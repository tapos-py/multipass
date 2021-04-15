/*
 * Copyright (C) 2019 Canonical, Ltd.
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

#include <multipass/exceptions/invalid_memory_size_exception.h>
#include <multipass/memory_size.h>
#include <multipass/utils.h>

#include <multipass/format.h>

namespace mp = multipass;

namespace
{
constexpr auto kilo = 1024LL;
constexpr auto mega = kilo * kilo;
constexpr auto giga = mega * kilo;

long long in_bytes(const std::string& mem_value)
{
    if (mem_value.empty())
        return 0LL;

    QRegExp matcher("(\\d+)([KMG])?B?", Qt::CaseInsensitive);

    if (matcher.exactMatch(QString::fromStdString(mem_value)))
    {
        auto val = matcher.cap(1).toLongLong(); // value is in the second capture (1st one is the whole match)
        const auto unit = matcher.cap(2);       // unit in the third capture (idem)
        if (!unit.isEmpty())
        {
            switch (unit.at(0).toLower().toLatin1())
            {
            case 'g':
                val *= giga;
                break;
            case 'm':
                val *= mega;
                break;
            case 'k':
                val *= kilo;
                break;
            default:
                assert(false && "Shouldn't be here (invalid unit)");
            }
        }

        return val;
    }

    throw mp::InvalidMemorySizeException{mem_value};
}
} // namespace

mp::MemorySize::MemorySize() : bytes{0LL}
{
}

mp::MemorySize::MemorySize(const std::string& val) : bytes{::in_bytes(val)}
{
}

long long mp::MemorySize::in_bytes() const noexcept
{
    return bytes;
}

long long mp::MemorySize::in_kilobytes() const noexcept
{
    return bytes / kilo; // integer division to floor
}

long long mp::MemorySize::in_megabytes() const noexcept
{
    return bytes / mega; // integer division to floor
}

long long mp::MemorySize::in_gigabytes() const noexcept
{
    return bytes / giga; // integer division to floor
}

bool mp::operator==(const MemorySize& a, const MemorySize& b)
{
    return a.bytes == b.bytes;
}

bool mp::operator!=(const MemorySize& a, const MemorySize& b)
{
    return a.bytes != b.bytes;
}

bool mp::operator<(const MemorySize& a, const MemorySize& b)
{
    return a.bytes < b.bytes;
}

bool mp::operator>(const MemorySize& a, const MemorySize& b)
{
    return a.bytes > b.bytes;
}

bool mp::operator<=(const MemorySize& a, const MemorySize& b)
{
    return a.bytes <= b.bytes;
}

bool mp::operator>=(const MemorySize& a, const MemorySize& b)
{
    return a.bytes >= b.bytes;
}
