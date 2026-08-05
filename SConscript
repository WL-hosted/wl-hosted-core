# RT-Thread scons integration for wl-hosted-core.
#
# MCU adapters that build with scons (e.g. wl-hosted-coproc-wch-rtt) pick this
# file up through their SConscript walk. It registers the portable coprocessor
# side of the core as one RT-Thread group: coproc-core, the wire protocol
# codec with the checked-in pre-generated nanopb sources, the RT-Thread OSAL
# backend and the RTT ulog log backend. The host side (host-core, posix OSAL,
# simulator IPC, tests) is intentionally excluded; CMake consumers are
# unaffected by this file.
from building import *

cwd = GetCurrentDir()

CPPPATH = [
    cwd + '/coproc-core/include',
    cwd + '/protocol/include',
    cwd + '/protocol/generated',
    cwd + '/protocol/third_party/nanopb',
    cwd + '/common/osal/include',
    cwd + '/common/log/include',
]

src = []
src += Glob('coproc-core/src/*.c')
src += Glob('protocol/src/*.c')
src += Glob('protocol/generated/*.pb.c')
src += Glob('protocol/third_party/nanopb/pb_*.c')
src += [cwd + '/common/osal/src/rtt_osal.c']
src += [cwd + '/common/log/src/log_backend_rtt_ulog.c']

# Select the RTT ulog log backend for every core source that includes
# wlh/log.h; adapter groups should define the same macro (their SConscript).
CPPDEFINES = ['WLH_LOG_BACKEND_RTT_ULOG']

group = DefineGroup('wlh_core', src, depend=[''], CPPPATH=CPPPATH, CPPDEFINES=CPPDEFINES)

Return('group')
