// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024 Takashi Sakamoto

#define TRACE_SYSTEM	firewire

#if !defined(_FIREWIRE_TRACE_EVENT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _FIREWIRE_TRACE_EVENT_H

#include <linux/tracepoint.h>
#include <linux/firewire.h>
#include "core.h"

#define show_issue_str(issue)						\
	__print_symbolic(issue,						\
		{FW_TRACE_BUS_RESET_ISSUE_INITIATE, "Initiate" },	\
		{FW_TRACE_BUS_RESET_ISSUE_SCHEDULE, "Schedule" },	\
		{FW_TRACE_BUS_RESET_ISSUE_POSTPONE, "Postpone" },	\
		{FW_TRACE_BUS_RESET_ISSUE_DETECT, "Detect" })

TRACE_EVENT(bus_reset,
	TP_PROTO(enum fw_trace_bus_reset_issue issue, bool short_reset),
	TP_ARGS(issue, short_reset),
	TP_STRUCT__entry(
		__field(int, issue)
		__field(bool, short_reset)
	),
	TP_fast_assign(
		__entry->issue = issue;
		__entry->short_reset = short_reset;
	),
	TP_printk(
		"issue=%s short_reset=%s",
		show_issue_str(__entry->issue),
		__entry->short_reset ? "true" : "false")
);

#endif // _FIREWIRE_TRACE_EVENT_H

#define TRACE_INCLUDE_PATH	.
#define TRACE_INCLUDE_FILE	trace
#include <trace/define_trace.h>
