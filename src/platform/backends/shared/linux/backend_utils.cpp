/*
 * Copyright (C) 2018-2021 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "backend_utils.h"
#include "dbus_wrappers.h"
#include "process_factory.h"
#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/memory_size.h>
#include <multipass/platform.h>
#include <multipass/process/process.h>
#include <multipass/process/qemuimg_process_spec.h>
#include <multipass/top_catch_all.h>
#include <multipass/utils.h>

#include <shared/shared_backend_utils.h>

#include <scope_guard.hpp>

#include <QCoreApplication>
#include <QDBusMetaType>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QSysInfo>
#include <QtDBus/QtDBus>

#include <cassert>
#include <chrono>
#include <exception>
#include <mutex>
#include <random>
#include <type_traits>

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpdbus = multipass::backend::dbus;

typedef QMap<QString, QVariantMap> VariantMapMap;
Q_DECLARE_METATYPE(VariantMapMap) // for DBus

namespace
{
std::default_random_engine gen;
std::uniform_int_distribution<int> dist{0, 255};
const auto nm_bus_name = QStringLiteral("org.freedesktop.NetworkManager");
const auto nm_root_obj = QStringLiteral("/org/freedesktop/NetworkManager");
const auto nm_root_ifc = QStringLiteral("org.freedesktop.NetworkManager");
const auto nm_settings_obj = QStringLiteral("/org/freedesktop/NetworkManager/Settings");
const auto nm_settings_ifc = QStringLiteral("org.freedesktop.NetworkManager.Settings");
const auto nm_connection_ifc = QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection");
constexpr auto max_bridge_name_len = 15; // maximum number of characters in a bridge name

bool subnet_used_locally(const std::string& subnet)
{
    // CLI equivalent: ip -4 route show | grep -q ${SUBNET}
    const auto output = QString::fromStdString(mp::utils::run_cmd_for_output("ip", {"-4", "route", "show"}));
    return output.contains(QString::fromStdString(subnet));
}

bool can_reach_gateway(const std::string& ip)
{
    return mp::utils::run_cmd_for_status("ping", {"-n", "-q", ip.c_str(), "-c", "-1", "-W", "1"});
}

auto virtual_switch_subnet(const QString& bridge_name)
{
    // CLI equivalent: ip -4 route show | grep ${BRIDGE_NAME} | cut -d ' ' -f1 | cut -d '.' -f1-3
    QString subnet;

    const auto output =
        QString::fromStdString(mp::utils::run_cmd_for_output("ip", {"-4", "route", "show"})).split('\n');
    for (const auto& line : output)
    {
        if (line.contains(bridge_name))
        {
            subnet = line.section('.', 0, 2);
            break;
        }
    }

    if (subnet.isNull())
    {
        mpl::log(mpl::Level::info, "daemon",
                 fmt::format("Unable to determine subnet for the {} subnet", qUtf8Printable(bridge_name)));
    }
    return subnet.toStdString();
}

const mpdbus::DBusConnection& get_checked_system_bus()
{
    const auto& system_bus = mpdbus::DBusProvider::instance().get_system_bus();
    if (!system_bus.is_connected())
        throw mp::backend::CreateBridgeException{"Failed to connect to D-Bus system bus", system_bus.last_error()};

    return system_bus;
}

std::unique_ptr<mpdbus::DBusInterface> get_checked_interface(const mpdbus::DBusConnection& bus, const QString& service,
                                                             const QString& path, const QString& interface)
{
    auto ret = bus.get_interface(service, path, interface);

    assert(ret);
    if (!ret->is_valid())
        throw mp::backend::CreateBridgeException{"Could not reach remote D-Bus object", ret->last_error()};

    return ret;
}

template <typename T, bool RollingBack = false, typename... Ts>
T checked_dbus_call(mpdbus::DBusInterface& interface, const QString& method_name, Ts&&... params)
{
    static constexpr auto error_template = "Failed DBus call. (Service: {}; Object: {}; Interface: {}; Method: {})";

    auto reply_msg = interface.call(QDBus::Block, method_name, QVariant::fromValue(std::forward<Ts>(params))...);
    QDBusReply<T> reply = reply_msg;

    if (!reply.isValid())
        throw mp::backend::CreateBridgeException{
            fmt::format(error_template, interface.service(), interface.path(), interface.interface(), method_name),
            reply.error(), RollingBack};

    if constexpr (!std::is_void_v<T>)
        return reply.value();
}

auto make_connection_settings(const QString& parent_name, const QString& child_name, const QString& interface_name)
{
    VariantMapMap arg1{{"connection", {{"type", "bridge"}, {"id", parent_name}, {"autoconnect-slaves", 1}}},
                       {"bridge", {{"interface-name", parent_name}}}};
    VariantMapMap arg2{{"connection",
                        {{"id", child_name},
                         {"type", "802-3-ethernet"},
                         {"slave-type", "bridge"},
                         {"master", parent_name},
                         {"interface-name", interface_name},
                         {"autoconnect-priority", 10}}}};

    return std::make_pair(arg1, arg2);
}

auto make_bridge_rollback_guard(const mpl::CString& log_category, const mpdbus::DBusConnection& system_bus,
                                const QDBusObjectPath& parent_path, const QDBusObjectPath& child_path)
{
    auto rollback = [&system_bus, &parent_path, &child_path] {
        for (auto& obj_path : {child_path, parent_path})
        {
            if (auto path = obj_path.path(); !path.isNull())
            {
                auto connection = system_bus.get_interface(nm_bus_name, path, nm_connection_ifc);
                checked_dbus_call<void, /* RollingBack = */ true>(*connection, "Delete");
            }
        }
    };

    return sg::make_scope_guard([rollback, log_category]() noexcept {
        mpl::log(mpl::Level::info, log_category, "Rolling back bridge");
        mp::top_catch_all(log_category, rollback);
    });
}

} // namespace

std::string mp::backend::generate_random_subnet()
{
    gen.seed(std::chrono::system_clock::now().time_since_epoch().count());
    for (auto i = 0; i < 100; ++i)
    {
        auto subnet = fmt::format("10.{}.{}", dist(gen), dist(gen));
        if (subnet_used_locally(subnet))
            continue;

        if (can_reach_gateway(fmt::format("{}.1", subnet)))
            continue;

        if (can_reach_gateway(fmt::format("{}.254", subnet)))
            continue;

        return subnet;
    }

    throw std::runtime_error("Could not determine a subnet for networking.");
}

std::string mp::backend::get_subnet(const mp::Path& network_dir, const QString& bridge_name)
{
    auto subnet = virtual_switch_subnet(bridge_name);
    if (!subnet.empty())
        return subnet;

    QFile subnet_file{network_dir + "/multipass_subnet"};
    subnet_file.open(QIODevice::ReadWrite | QIODevice::Text);
    if (subnet_file.size() > 0)
        return subnet_file.readAll().trimmed().toStdString();

    auto new_subnet = mp::backend::generate_random_subnet();
    subnet_file.write(new_subnet.data(), new_subnet.length());
    return new_subnet;
}

void mp::backend::resize_instance_image(const MemorySize& disk_space, const mp::Path& image_path)
{
    auto disk_size = QString::number(disk_space.in_bytes()); // format documented in `man qemu-img` (look for "size")
    QStringList qemuimg_parameters{{"resize", image_path, disk_size}};
    auto qemuimg_process =
        mp::platform::make_process(std::make_unique<mp::QemuImgProcessSpec>(qemuimg_parameters, "", image_path));

    auto process_state = qemuimg_process->execute(mp::backend::image_resize_timeout);
    if (!process_state.completed_successfully())
    {
        throw std::runtime_error(fmt::format("Cannot resize instance image: qemu-img failed ({}) with output:\n{}",
                                             process_state.failure_message(),
                                             qemuimg_process->read_all_standard_error()));
    }
}

mp::Path mp::backend::convert_to_qcow_if_necessary(const mp::Path& image_path)
{
    // Check if raw image file, and if so, convert to qcow2 format.
    // TODO: we could support converting from other the image formats that qemu-img can deal with
    const auto qcow2_path{image_path + ".qcow2"};

    auto qemuimg_info_spec =
        std::make_unique<mp::QemuImgProcessSpec>(QStringList{"info", "--output=json", image_path}, image_path);
    auto qemuimg_info_process = MP_PROCFACTORY.create_process(std::move(qemuimg_info_spec));

    auto process_state = qemuimg_info_process->execute();
    if (!process_state.completed_successfully())
    {
        throw std::runtime_error(fmt::format("Cannot read image format: qemu-img failed ({}) with output:\n{}",
                                             process_state.failure_message(),
                                             qemuimg_info_process->read_all_standard_error()));
    }

    auto image_info = qemuimg_info_process->read_all_standard_output();
    auto image_record = QJsonDocument::fromJson(QString(image_info).toUtf8(), nullptr).object();

    if (image_record["format"].toString() == "raw")
    {
        auto qemuimg_convert_spec = std::make_unique<mp::QemuImgProcessSpec>(
            QStringList{"convert", "-p", "-O", "qcow2", image_path, qcow2_path}, image_path, qcow2_path);
        auto qemuimg_convert_process = MP_PROCFACTORY.create_process(std::move(qemuimg_convert_spec));
        process_state = qemuimg_convert_process->execute(mp::backend::image_resize_timeout);

        if (!process_state.completed_successfully())
        {
            throw std::runtime_error(
                fmt::format("Failed to convert image format: qemu-img failed ({}) with output:\n{}",
                            process_state.failure_message(), qemuimg_convert_process->read_all_standard_error()));
        }
        return qcow2_path;
    }
    else
    {
        return image_path;
    }
}

QString mp::backend::cpu_arch()
{
    const QHash<QString, QString> cpu_to_arch{{"x86_64", "x86_64"}, {"arm", "arm"},   {"arm64", "aarch64"},
                                              {"i386", "i386"},     {"power", "ppc"}, {"power64", "ppc64le"},
                                              {"s390x", "s390x"}};

    return cpu_to_arch.value(QSysInfo::currentCpuArchitecture());
}

void mp::backend::check_for_kvm_support()
{
    QProcess check_kvm;
    check_kvm.setProcessChannelMode(QProcess::MergedChannels);
    check_kvm.start(QDir(QCoreApplication::applicationDirPath()).filePath("check_kvm_support"));
    check_kvm.waitForFinished();

    if (check_kvm.error() == QProcess::FailedToStart)
    {
        throw std::runtime_error("The check_kvm_support script failed to start. Ensure it is in multipassd's PATH.");
    }

    if (check_kvm.exitCode() == 1)
    {
        throw std::runtime_error(check_kvm.readAll().trimmed().toStdString());
    }
}

void mp::backend::check_if_kvm_is_in_use()
{
    auto kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    auto ret = ioctl(kvm_fd, KVM_CREATE_VM, (unsigned long)0);

    close(kvm_fd);

    if (ret == -1 && errno == EBUSY)
        throw std::runtime_error("Another virtual machine manager is currently running. Please shut it down before "
                                 "starting a Multipass instance.");

    close(ret);
}

// @precondition no bridge exists for this interface
// @precondition interface identifies an ethernet device
void mp::backend::create_bridge_with(const std::string& interface)
{
    static constexpr auto log_category_create = "create bridge";
    static constexpr auto log_category_rollback = "rollback bridge";
    static const auto root_path = QDBusObjectPath{"/"};
    static const auto base_name = QStringLiteral("br-");

    static std::once_flag once;
    std::call_once(once, [] { qDBusRegisterMetaType<VariantMapMap>(); });

    const auto& system_bus = get_checked_system_bus();
    auto nm_root = get_checked_interface(system_bus, nm_bus_name, nm_root_obj, nm_root_ifc);
    auto nm_settings = get_checked_interface(system_bus, nm_bus_name, nm_settings_obj, nm_settings_ifc);

    auto parent_name = (base_name + interface.c_str()).left(max_bridge_name_len);
    auto child_name = parent_name + "-child";
    mpl::log(mpl::Level::debug, log_category_create, fmt::format("Creating bridge: {}", parent_name));

    // AddConnection expects the following DBus argument type: a{sa{sv}}
    const auto& [arg1, arg2] = make_connection_settings(parent_name, child_name, QString::fromStdString(interface));

    // The rollbacks could be achieved with
    //   `nmcli connection delete <parent_connection> <child_connection>
    QDBusObjectPath parent_path{}, child_path{};
    auto rollback_guard = // rollback unless we succeed
        make_bridge_rollback_guard(log_category_rollback, system_bus, parent_path, child_path);

    // The following DBus calls are roughly equivalent to:
    //   `nmcli connection add type bridge ifname <br> connection.autoconnect-slaves 1`
    //   `nmcli connection add type bridge-slave ifname <if> master <br> connection.autoconnect-priority 10`
    //   `nmcli connection up <child_connection>`
    parent_path = checked_dbus_call<QDBusObjectPath>(*nm_settings, "AddConnection", arg1);
    child_path = checked_dbus_call<QDBusObjectPath>(*nm_settings, "AddConnection", arg2);
    checked_dbus_call<QDBusObjectPath>(*nm_root, "ActivateConnection", child_path, root_path, root_path); /* Inspiration
    for '/' to signal null `device` and `specific-object` derived from nmcli and libnm. See https://bit.ly/3dMA3QB */

    rollback_guard.dismiss(); // we succeeded!
    mpl::log(mpl::Level::info, log_category_create, fmt::format("Created bridge: {}", parent_name));
}

mp::backend::CreateBridgeException::CreateBridgeException(const std::string& detail, const QDBusError& dbus_error,
                                                          bool rollback)
    : std::runtime_error(fmt::format("{}. {}: {}", rollback ? "Could not rollback bridge" : "Could not create bridge",
                                     detail, dbus_error.isValid() ? dbus_error.message() : "unknown cause"))
{
}
