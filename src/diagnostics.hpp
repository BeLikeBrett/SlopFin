/* SlopFin - optional diagnostic upload destination.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_DIAGNOSTICS_HPP
#define SLOPFIN_DIAGNOSTICS_HPP
#include "server_address.hpp"
#include <string_view>
namespace slopfin::diagnostics
{
inline server_address::Address destination(std::string_view configured)
{
    return server_address::parse(configured);
}
} // namespace slopfin::diagnostics
#endif
