#pragma once

// One DNS task, bound only to the setup AP. Stop releases its socket before a normal reopen.
void setup_dns_start(void);
void setup_dns_stop(void);
