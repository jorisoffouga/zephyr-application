# SPDX-License-Identifier: Apache-2.0

# Keep the resource table even when the application never uses it (RAM
# console only): Linux remoteproc reads the trace entry from it.
zephyr_linker_sources_ifdef(CONFIG_OPENAMP_RSC_TABLE ROM_SECTIONS rsc_table.ld)
