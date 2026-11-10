// CopicatClap.h
// The three entry functions clap-wrapper links into every format.

#pragma once

bool tbcc_entry_init (const char* pluginPath);
void tbcc_entry_deinit();
const void* tbcc_entry_get_factory (const char* factoryId);
