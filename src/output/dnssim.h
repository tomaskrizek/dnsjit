/*
 * Copyright (c) 2019, CZ.NIC, z.s.p.o.
 * All rights reserved.
 *
 * This file is part of dnsjit.
 *
 * dnsjit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dnsjit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dnsjit.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/object/dns.h"
#include "core/object/ip.h"
#include "core/object/ip6.h"
#include "core/object/payload.h"
#include "core/producer.h"
#include "core/receiver.h"

#ifndef __dnsjit_output_dnssim_h
#define __dnsjit_output_dnssim_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>
#include <ck_ring.h>

#include "output/dnssim.hh"
#include "output/dnssim/internal.h"
#include "output/dnssim/ll.h"

#endif
