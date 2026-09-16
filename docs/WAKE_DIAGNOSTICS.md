# Wake diagnostics

Each boot prints the prior wake's reset reason, wake source, last completed
phase, phase in progress, minimum sampled task stack headroom (in bytes), and
last Wi-Fi disconnect reason. The same line is emitted again after persistent
debug logging starts. If storage or configuration initialization fails, the
early console line is still available.

The record uses two checksum-protected slots in RTC memory (120 bytes total).
Phase and disconnect updates write only RTC memory; they do not write flash or
storage. A power loss can erase the record. A reset during an update should
leave the last complete slot available. A missing or corrupt record is reported
as unavailable.

`completed` is the last phase that returned normally; `active` identifies a
phase that had started but not completed. `main_stack_min` is the minimum main
task headroom sampled at phase boundaries. `stack_min` is the minimum across
tasks that passed through those boundaries, with its task name. These samples
are historical high-water marks, not instantaneous free stack. The Wi-Fi reason
is a numeric `wifi_err_reason_t` value; zero means no disconnect event was seen.

For a debug build with flash coredumps, build with:

```sh
python3 build.py --board seeedstudio_xiao_ee02 --debug
```

The debug overlay changes the partition table. A valid coredump logs the
crashed task, PC, and backtrace on the next boot; resolve addresses against the
matching ELF with `xtensa-esp32s3-elf-addr2line`. Missing dumps are logged, and
corrupt or unsummarizable dumps are retained for offline inspection.

## Device validation

On each target board, record the firmware version and complete serial log for:

1. A normal timer wake that rotates and reaches deep sleep. The next boot
   should report `completed=sleep` and reset reason `ESP_RST_DEEPSLEEP` (numeric
   value in the diagnostics line; the normal reset log prints the name).
2. A forced assertion during Wi-Fi or rotation in a debug build. The next boot
   should identify the active phase and print a coredump task/backtrace.
3. A deliberately undersized test task that overflows in a debug build. Verify
   the coredump and that its task name is reported.
4. Storage unavailable at boot. Verify the early serial diagnostics line.
5. No coredump and an invalid coredump image. Verify that boot continues and
   the respective status appears in logs.

Remove any assertion or overflow test code before shipping a build.
