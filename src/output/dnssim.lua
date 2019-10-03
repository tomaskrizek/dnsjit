-- Copyright (c) 2018-2019, CZ.NIC, z.s.p.o.
-- All rights reserved.
--
-- This file is part of dnsjit.
--
-- dnsjit is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- dnsjit is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with dnsjit.  If not, see <http://www.gnu.org/licenses/>.

-- dnsjit.output.dnssim
-- Simulate independent DNS clients over various transports
--   TODO
--
-- Output module for simulating traffic from huge number of independent,
-- individual DNS clients. Uses libuv for asynchronous communication. There
-- may only be a single dnssim in a thread. Use dnsjit.core.thread to have
-- multiple dnssim instances.
module(...,package.seeall)

require("dnsjit.output.dnssim_h")
local bit = require("bit")
local object = require("dnsjit.core.objects")
local ffi = require("ffi")
local C = ffi.C

local DnsSim = {}

-- Create a new DnsSim output for up to max_clients.
function DnsSim.new(max_clients)
    local self = {
        obj = C.output_dnssim_new(max_clients),
        max_clients = max_clients,
    }
    ffi.gc(self.obj, C.output_dnssim_free)
    return setmetatable(self, { __index = DnsSim })
end

-- Return the Log object to control logging of this instance or module.
function DnsSim:log()
    if self == nil then
        return C.output_dnssim_log()
    end
    return self.obj._log
end

-- Set the transport to UDP (without any TCP fallback).
function DnsSim:udp_only()
    C.output_dnssim_set_transport(self.obj, C.OUTPUT_DNSSIM_TRANSPORT_UDP_ONLY)
end

-- Set the preferred transport to UDP. This transport falls back to TCP
-- for individual queries if TC bit is set in received answer.
function DnsSim:udp()
    C.output_dnssim_set_transport(self.obj, C.OUTPUT_DNSSIM_TRANSPORT_UDP)
    self.obj.transport = "OUTPUT_DNSSIM_TRANSPORT_UDP"
end

-- Set the transport to TCP.
function DnsSim:tcp()
    C.output_dnssim_set_transport(self.obj, C.OUTPUT_DNSSIM_TRANSPORT_TCP)
    self.obj.transport = "OUTPUT_DNSSIM_TRANSPORT_TCP"
end

-- Set the transport to TLS.
function DnsSim:tls()
    C.output_dnssim_set_transport(self.obj, C.OUTPUT_DNSSIM_TRANSPORT_TLS)
end

-- Set timeout for the individual requests in seconds (default 2s).
function DnsSim:timeout(seconds)
    if seconds == nil then
        seconds = 2
    end
    local timeout_ms = math.floor(seconds * 1000)
    self.obj.timeout_ms = math.floor(seconds * 1000)
end

-- Configure statistics to be collected every N seconds.
function DnsSim:stats_collect(seconds)
    if seconds == nil then
        self.obj._log:critical("number of seconds must be set for stats_collect()")
    end
    interval_ms = math.floor(seconds * 1000)
    C.output_dnssim_stats_collect(self.obj, interval_ms)
end

-- Stop the collection of statistics.
function DnsSim:stats_finish()
    C.output_dnssim_stats_finish(self.obj)
end

-- Set this to true if dnssim should free the memory of passed-in objects (useful
-- when using copy() to pass objects from different thread).
function DnsSim:free_after_use(free_after_use)
    self.obj.free_after_use = free_after_use
end

-- Return the C function and context for receiving objects.
function DnsSim:receive()
    local receive = C.output_dnssim_receiver()
    return receive, self.obj
end

-- Set the target server where queries will be sent to. Returns 0 on success.
function DnsSim:target(ip, port)
    local nport = tonumber(port)
    if nport == nil then
        self.obj._log:critical("invalid port: "..port)
        return -1
    end
    if nport <= 0 or nport > 65535 then
        self.obj._log:critical("invalid port number: "..nport)
        return -1
    end
    return C.output_dnssim_target(self.obj, ip, nport)
end

-- Specify source address for sending queries. Can be set multiple times. Adresses
-- are selected round-robin when sending.
function DnsSim:bind(ip)
    return C.output_dnssim_bind(self.obj, ip)
end

-- Run the libuv loop once without blocking when there is no I/O. This
-- should be called repeatedly until 0 is returned and no more data
-- is expected to be received by DnsSim.
function DnsSim:run_nowait()
    return C.output_dnssim_run_nowait(self.obj)
end

-- Number of input packets discarded due to various reasons.
-- To investigate causes, run with increased logging level.
function DnsSim:discarded()
    return tonumber(self.obj.discarded)
end

-- Number of valid requests (input packets) processed.
function DnsSim:requests()
    return tonumber(self.obj.stats_sum.requests)
end

-- Number of requests that received an answer
function DnsSim:answers()
    return tonumber(self.obj.stats_sum.answers)
end

-- Number of requests that received a NOERROR response
function DnsSim:noerror()
    return tonumber(self.obj.stats_sum.noerror)
end

-- Export the results to a JSON file
function DnsSim:export(filename)
    local file = io.open(filename, "w")
    if file == nil then
        self.obj._log:critical("export failed: no filename")
        return
    end

    local function write_stats(file, stats)
        file:write(
            "{ ",
                '"requests": ', tonumber(stats.requests), ', ',
                '"ongoing": ', tonumber(stats.ongoing), ', ',
                '"answers": ', tonumber(stats.answers), ', ',
                '"noerror": ', tonumber(stats.noerror),
            "}")
    end

    file:write(
        "{ ",
            '"discarded": ', self:discarded(), ', ',
            '"stats_sum": ')
    write_stats(file, self.obj.stats_sum)
    file:write(
            ', ',
            '"stats_periodic": [')

    local stats = self.obj.stats_first
    write_stats(file, stats)

    while (stats.next ~= nil) do
        stats = stats.next
        file:write(', ')
        write_stats(file, stats)
    end

    file:write(']}')
    file:close()
    self.obj._log:notice("results exported to "..filename)
end

return DnsSim
