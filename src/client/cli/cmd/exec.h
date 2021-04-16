/*
 * Copyright (C) 2017-2018 Canonical, Ltd.
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
 * Authored by: Chris Townsend <christopher.townsend@canonical.com>
 *
 */

#ifndef MULTIPASS_EXEC_H
#define MULTIPASS_EXEC_H

#include <multipass/cli/alias_dict.h>
#include <multipass/cli/command.h>

namespace multipass
{
namespace cmd
{
class Exec final : public Command
{
public:
    using Command::Command;

    Exec(grpc::Channel& channel, Rpc::Stub& stub, Terminal* term, AliasDict& dict)
        : Command(channel, stub, term), aliases(dict)
    {
    }

    ReturnCode run(ArgParser* parser) override;
    std::string name() const override;
    QString short_help() const override;
    QString description() const override;

    static ReturnCode exec_success(const SSHInfoReply& reply, const std::vector<std::string>& args, Terminal* term);

private:
    SSHInfoRequest request;
    AliasDict aliases;

    ParseCode parse_args(ArgParser* parser) override;
};
} // namespace cmd
} // namespace multipass
#endif // MULTIPASS_EXEC_H
