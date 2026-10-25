if(NOT DEFINED SOURCE OR SOURCE STREQUAL "")
    message(FATAL_ERROR "instrument-guest-probe.cmake requires SOURCE")
endif()
if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "Generated guest probe shard does not exist: ${SOURCE}")
endif()

file(READ "${SOURCE}" _source)
set(_edf_probe_changed FALSE)
string(FIND "${_source}" "#if defined(EDF2027_GUEST_PROBE_TRACE)" _trace_guard_pos)
if(_trace_guard_pos EQUAL -1)
    string(FIND "${_source}" "edf::net::probe::RecordRawVmDispatch" _raw_callback_pos)
    string(FIND "${_source}" "edf::net::probe::RecordMissionStartDispatch" _mission_callback_pos)
    if(NOT _raw_callback_pos EQUAL -1 OR NOT _mission_callback_pos EQUAL -1)
        message(FATAL_ERROR "Generated guest probe shard contains unguarded or partial instrumentation")
    endif()
    set(_edf_source_has_trace_guards FALSE)
else()
    set(_edf_source_has_trace_guards TRUE)
endif()

# Accept exactly one raw anchor or exactly one already-patched sequence. Anything
# ambiguous means codegen moved the target function and should stop the build.
function(_edf_replace_once variable label before after)
    set(_text "${${variable}}")
    string(REPLACE "${after}" "__EDF_PROBE_AFTER_MARKER__" _without_after "${_text}")
    string(REGEX MATCHALL "__EDF_PROBE_AFTER_MARKER__" _after_markers "${_without_after}")
    list(LENGTH _after_markers _after_count)
    string(REPLACE "${before}" "__EDF_PROBE_BEFORE_MARKER__" _counted "${_without_after}")
    string(REGEX MATCHALL "__EDF_PROBE_BEFORE_MARKER__" _before_markers "${_counted}")
    list(LENGTH _before_markers _before_count)
    if(_after_count GREATER 0)
        if(NOT _after_count EQUAL 1 OR NOT _before_count EQUAL 0)
            message(FATAL_ERROR "Generated probe instrumentation is duplicated or malformed at ${label}")
        endif()
    elseif(_edf_source_has_trace_guards)
        message(FATAL_ERROR "Generated probe instrumentation is partially present at ${label}")
    elseif(_before_count GREATER 0)
        if(NOT _before_count EQUAL 1)
            message(FATAL_ERROR "Generated function layout has duplicate anchor at ${label}")
        endif()
        string(REPLACE "${before}" "${after}" _updated "${_text}")
        set(${variable} "${_updated}" PARENT_SCOPE)
        set(_edf_probe_changed TRUE PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Generated function layout changed at ${label}; expected probe anchor was not found")
    endif()
endfunction()

set(_declarations_after [=[#include "edf2017_funcs.65.h"

#if defined(EDF2027_GUEST_PROBE_TRACE)
namespace edf::net::probe {
void RecordRawVmDispatch(uint8_t* base, uint32_t context, uint32_t opcode,
                         uint32_t phase, uint32_t event, uint32_t target,
                         uint32_t callsite);
void RecordMissionStartDispatch(uint8_t* base, uint32_t context, uint32_t opcode_pc,
                               uint32_t fallthrough_pc, uint32_t event,
                               uint32_t callback_target, uint32_t retry_byte_before,
                               uint32_t retry_count_before, bool retry_branch_taken);
}
#endif

]=])
_edf_replace_once(_source "shard declarations"
    [=[#include "edf2017_funcs.65.h"

]=]
    "${_declarations_after}")

string(FIND "${_source}" "DEFINE_REX_FUNC(sub_821DE3B0) {" _function_start)
if(_function_start EQUAL -1)
    message(FATAL_ERROR "Generated function layout changed: sub_821DE3B0 is missing from edf2017_recomp.65.cpp")
endif()
string(REPLACE "DEFINE_REX_FUNC(sub_821DE3B0) {" "__EDF_PROBE_FUNCTION_MARKER__"
    _function_marked "${_source}")
string(REGEX MATCHALL "__EDF_PROBE_FUNCTION_MARKER__" _function_matches "${_function_marked}")
list(LENGTH _function_matches _function_count)
if(NOT _function_count EQUAL 1)
    message(FATAL_ERROR "Generated function layout changed: expected one sub_821DE3B0, found ${_function_count}")
endif()
string(SUBSTRING "${_source}" ${_function_start} -1 _function_and_tail)
string(FIND "${_function_and_tail}" "\nDEFINE_REX_FUNC(" _next_function)
if(_next_function EQUAL -1)
    message(FATAL_ERROR "Generated function layout changed: cannot find the end of sub_821DE3B0")
endif()
string(SUBSTRING "${_function_and_tail}" 0 ${_next_function} _function)
math(EXPR _tail_start "${_function_start} + ${_next_function}")
string(SUBSTRING "${_source}" 0 ${_function_start} _prefix)
string(SUBSTRING "${_source}" ${_tail_start} -1 _tail)

_edf_replace_once(_function "sub_821DE3B0 local declarations"
    [=[	PPCRegister temp{};
	uint32_t ea{};
]=]
    [=[	PPCRegister temp{};
	uint32_t ea{};
#if defined(EDF2027_GUEST_PROBE_TRACE)
	uint32_t opcode_pc = 0;
	uint32_t fallthrough_pc = 0;
	uint32_t callback_target = 0;
	uint32_t dispatched_event = UINT32_MAX;
	uint32_t retry_byte_before = 0;
	uint32_t retry_count_before = 0;
	bool retry_branch_taken = false;
#endif
]=])

_edf_replace_once(_function "sub_821DE3B0 opcode capture"
    [=[	r26.u64 = REX_LOAD_U32(r29.u32 + 0);
]=]
    [=[	r26.u64 = REX_LOAD_U32(r29.u32 + 0);
#if defined(EDF2027_GUEST_PROBE_TRACE)
	opcode_pc = r26.u32;
#endif
]=])

_edf_replace_once(_function "sub_821DE3B0 raw VM dispatch"
    [=[	ctx.lr = lr;
	REX_CALL_INDIRECT_FUNC(ctr.u32);
]=]
    [=[	ctx.lr = lr;
#if defined(EDF2027_GUEST_PROBE_TRACE)
	callback_target = ctr.u32;
	dispatched_event = r7.u32 & 0x1Fu;
	edf::net::probe::RecordRawVmDispatch(base, r28.u32, r25.u32, r6.u32, r7.u32,
	                                     ctr.u32, 0x821DF1B8);
#endif
	REX_CALL_INDIRECT_FUNC(ctr.u32);
]=])

_edf_replace_once(_function "sub_821DE3B0 retry-state capture"
    [=[	r11.u64 = REX_LOAD_U8(r28.u32 + 60);
]=]
    [=[	r11.u64 = REX_LOAD_U8(r28.u32 + 60);
#if defined(EDF2027_GUEST_PROBE_TRACE)
	retry_byte_before = r11.u32;
	retry_count_before = REX_LOAD_U32(r28.u32 + 72);
	retry_branch_taken = retry_byte_before != 0;
#endif
]=])

_edf_replace_once(_function "sub_821DE3B0 mission-start dispatch"
    [=[loc_821DF3C8:
	// lwz r11,0(r28)
]=]
    [=[loc_821DF3C8:
#if defined(EDF2027_GUEST_PROBE_TRACE)
	fallthrough_pc = REX_LOAD_U32(r28.u32 + 164);
	edf::net::probe::RecordMissionStartDispatch(
	    base, r28.u32, opcode_pc, fallthrough_pc, dispatched_event,
	    callback_target, retry_byte_before, retry_count_before, retry_branch_taken);
#endif
	// lwz r11,0(r28)
]=])

set(_source "${_prefix}${_function}${_tail}")
if(_edf_probe_changed)
    file(WRITE "${SOURCE}" "${_source}")
    message(STATUS "Instrumented generated guest probe dispatches in ${SOURCE}")
else()
    message(STATUS "Generated guest probe dispatches already have the expected instrumentation in ${SOURCE}")
endif()
