#pragma once

/*
 * tcmd_atom_echo — TRIGGERcmd ATOM Echo firmware variant.
 *
 * Entry point called from app_main() when CONFIG_HARDWARE_TCMD_ATOM_ECHO=y.
 * This function never returns.
 */
void tcmd_atom_echo_run(void);
