# BE300Probe v4 Cluster Report

- runs_parsed: `6`
- all_runs_probe_complete_ok: `True`
- any_old_0f0200_fault: `False`

## Run Table

| File                             | Tag     | Status | NAND                                     | Storage                                   | 0F0200 | UTF16             |
| -------------------------------- | ------- | ------ | ---------------------------------------- | ----------------------------------------- | ------ | ----------------- |
| BE300Probe_v4_cold_22664.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | X?                |
| BE300Probe_v4_cold_26319.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | imodem.dll        |
| BE300Probe_v4_unknown_112248.txt | unknown | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\HW_SURVEY.EXE | NO     | X?                |
| BE300Probe_v4_warm_11583.txt     | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | imodem.dll        |
| BE300Probe_v4_warm_81147.txt     | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | imodem.dll        |
| BE300Probe_v4_warm_9550.txt      | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | FileCalendar.exe |

## Stable Across All Current Runs And Phases

- `region_crc:callback_table_51600`
- `region_crc:vr4131_bcu_window`
- `focus_word:nand_c30`
- `focus_word:nand_c34`
- `focus_word:nand_c48`
- `focus_word:nand_c4c`
- `focus_word:icu_0080`
- `focus_word:stack_1760`
- `focus_word:stack_1764`
- `focus_word:stack_179C`
- `focus_word:stack_17B4`

## Phase-Varying Within At Least One Run

- `region_crc:stack_frame_1700` correlation=per_run_unique
- `region_crc:vrc4173_base` correlation=per_run_unique
- `region_crc:vr4131_icu_pmu_window` correlation=per_run_unique
- `focus_word:nand_sideband_c38` correlation=per_run_unique
- `focus_word:pmu_0100` correlation=per_run_unique

## Multiple Families Across Runs

- `region_crc:resume_context_2200` families=3 correlation=no_clear_correlation
  - `0xDD851290/0xDD851290/0xDD851290` -> BE300Probe_v4_cold_22664.txt
  - `0xD84146E5/0xD84146E5/0xD84146E5` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0xE187199B/0xE187199B/0xE187199B` -> BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
- `region_crc:stack_frame_1700` families=6 correlation=per_run_unique
  - `0xDEEA5523/0x1BB7BB79/0x6EE6F9D8` -> BE300Probe_v4_cold_22664.txt
  - `0x92D6ABD8/0x46855385/0x273093E5` -> BE300Probe_v4_cold_26319.txt
  - `0x2C160B14/0x2C1B0588/0xD5B4B135` -> BE300Probe_v4_unknown_112248.txt
  - `0x0D4EC77F/0x20A44D0D/0x0DBB14C9` -> BE300Probe_v4_warm_11583.txt
  - `0xBD6CA6AA/0x8E4F911A/0x2989D704` -> BE300Probe_v4_warm_81147.txt
  - `0x0DABB6EF/0x08081FF0/0x7958B785` -> BE300Probe_v4_warm_9550.txt
- `region_crc:callback_globals_80679400` families=3 correlation=no_clear_correlation
  - `0x209F41C0/0x209F41C0/0x209F41C0` -> BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x76516A1A/0x76516A1A/0x76516A1A` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x33D26683/0x33D26683/0x33D26683` -> BE300Probe_v4_warm_9550.txt
- `region_crc:vrc4173_base` families=6 correlation=per_run_unique
  - `0x4221B52C/0x421F368F/0x3D5FCDE0` -> BE300Probe_v4_cold_22664.txt
  - `0x6D431D0F/0x08BFE493/0x91E69504` -> BE300Probe_v4_cold_26319.txt
  - `0x1CC0B976/0xB640900F/0x297A506E` -> BE300Probe_v4_unknown_112248.txt
  - `0x316D13D1/0x425BC2D3/0xD9324397` -> BE300Probe_v4_warm_11583.txt
  - `0x92E24043/0x8B926D28/0x0F0F2511` -> BE300Probe_v4_warm_81147.txt
  - `0x7BA7E207/0x2ED7BDDD/0xC05E7D7E` -> BE300Probe_v4_warm_9550.txt
- `region_crc:vr4131_icu_pmu_window` families=6 correlation=per_run_unique
  - `0xD754B513/0x38375F1F/0x1A0D9F29` -> BE300Probe_v4_cold_22664.txt
  - `0x07F38927/0xA96DE276/0x703F3808` -> BE300Probe_v4_cold_26319.txt
  - `0xBD534B30/0x9BAFBD4D/0x509BB7F3` -> BE300Probe_v4_unknown_112248.txt
  - `0xC6CFFC20/0x2B575506/0x49021CBD` -> BE300Probe_v4_warm_11583.txt
  - `0xB9CDC8C1/0x96289E2D/0x2C1ACA9F` -> BE300Probe_v4_warm_81147.txt
  - `0x47EFDB7F/0xD37B183B/0xA0839362` -> BE300Probe_v4_warm_9550.txt
- `focus_word:nand_sideband_c38` families=6 correlation=per_run_unique
  - `0x000000CE/0x0000004E/0x0000009A` -> BE300Probe_v4_cold_22664.txt
  - `0x00000037/0x000000BB/0x00000072` -> BE300Probe_v4_cold_26319.txt
  - `0x00000035/0x00000077/0x00000051` -> BE300Probe_v4_unknown_112248.txt
  - `0x00000057/0x0000009D/0x0000003F` -> BE300Probe_v4_warm_11583.txt
  - `0x00000011/0x00000086/0x000000D6` -> BE300Probe_v4_warm_81147.txt
  - `0x00000063/0x000000CC/0x00000047` -> BE300Probe_v4_warm_9550.txt
- `focus_word:pmu_0100` families=6 correlation=per_run_unique
  - `0x14051DC9/0x1406BC93/0x14071B59` -> BE300Probe_v4_cold_22664.txt
  - `0x1404E281/0x140684C4/0x1406E65A` -> BE300Probe_v4_cold_26319.txt
  - `0x9C354C70/0x9C36F008/0x9C374F59` -> BE300Probe_v4_unknown_112248.txt
  - `0x9C5653B9/0x9C57F43E/0x9C585252` -> BE300Probe_v4_warm_11583.txt
  - `0x14202D93/0x14220C5D/0x14227733` -> BE300Probe_v4_warm_81147.txt
  - `0x14313960/0x1432D852/0x143339B0` -> BE300Probe_v4_warm_9550.txt
- `focus_word:ctx_2220` families=3 correlation=no_clear_correlation
  - `0x00814004/0x00814004/0x00814004` -> BE300Probe_v4_cold_22664.txt
  - `0x00814000/0x00814000/0x00814000` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
- `focus_word:ctx_2228` families=3 correlation=no_clear_correlation
  - `0x10908040/0x10908040/0x10908040` -> BE300Probe_v4_cold_22664.txt
  - `0x00808000/0x00808000/0x00808000` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
- `focus_word:ctx_2274` families=3 correlation=no_clear_correlation
  - `0x00012090/0x00012090/0x00012090` -> BE300Probe_v4_cold_22664.txt
  - `0x00002010/0x00002010/0x00002010` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
- `focus_word:stack_1788` families=6 correlation=per_run_unique
  - `0x80DFCC8C/0x80DFCC8C/0x80DFCC8C` -> BE300Probe_v4_cold_22664.txt
  - `0x80DFB8FC/0x80DFB8FC/0x80DFB8FC` -> BE300Probe_v4_cold_26319.txt
  - `0x80D05AD8/0x80D05AD8/0x80D05AD8` -> BE300Probe_v4_unknown_112248.txt
  - `0x80DF1400/0x80DF1400/0x80DF1400` -> BE300Probe_v4_warm_11583.txt
  - `0x80E0B764/0x80E0B764/0x80E0B764` -> BE300Probe_v4_warm_81147.txt
  - `0x80F6CA20/0x80F6CA20/0x80F6CA20` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g94EC` families=3 correlation=no_clear_correlation
  - `0x0000002F/0x0000002F/0x0000002F` -> BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x0064002E/0x0064002E/0x0064002E` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x006C0061/0x006C0061/0x006C0061` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g94F0` families=3 correlation=no_clear_correlation
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x006C006C/0x006C006C/0x006C006C` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x006E0065/0x006E0065/0x006E0065` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g9508` families=3 correlation=no_clear_correlation
  - `0x002D0030/0x002D0030/0x002D0030` -> BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0xC0000014/0xC0000014/0xC0000014` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g9510` families=3 correlation=no_clear_correlation
  - `0x00360036/0x00360036/0x00360036` -> BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x004E0006/0x004E0006/0x004E0006` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x000004C7/0x000004C7/0x000004C7` -> BE300Probe_v4_warm_9550.txt

