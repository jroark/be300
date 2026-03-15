# BE300Probe v4 Cluster Report

- runs_parsed: `12`
- all_runs_probe_complete_ok: `True`
- any_old_0f0200_fault: `False`

## Run Table

| File                             | Tag     | Status | NAND                                     | Storage                                   | 0F0200 | UTF16             |
| -------------------------------- | ------- | ------ | ---------------------------------------- | ----------------------------------------- | ------ | ----------------- |
| BE300Probe_v4_cold_20091.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | imodem.dll        |
| BE300Probe_v4_cold_21463.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | X?                |
| BE300Probe_v4_cold_21656.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | <conv-failed>     |
| BE300Probe_v4_cold_22664.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | X?                |
| BE300Probe_v4_cold_26319.txt     | cold    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | imodem.dll        |
| BE300Probe_v4_unknown_112248.txt | unknown | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\HW_SURVEY.EXE | NO     | X?                |
| BE300Probe_v4_warm_11583.txt     | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | imodem.dll        |
| BE300Probe_v4_warm_11956.txt     | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | ileCalendar.exe   |
| BE300Probe_v4_warm_14709.txt     | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | ileCalendar.exe   |
| BE300Probe_v4_warm_15212.txt     | warm    | OK     | \Nand Disk\Program Files\ieceext.dll.cpk | \Storage Card\Program Files\hw_survey.exe | NO     | ileCalendar.exe   |
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
- `focus_word:stack_179C`
- `focus_word:stack_17B4`

## Phase-Varying Within At Least One Run

- `region_crc:stack_frame_1700` correlation=per_run_unique
- `region_crc:vrc4173_base` correlation=per_run_unique
- `region_crc:vr4131_icu_pmu_window` correlation=per_run_unique
- `focus_word:nand_sideband_c38` correlation=per_run_unique
- `focus_word:pmu_0100` correlation=per_run_unique
- `focus_word:stack_1760` correlation=no_clear_correlation
- `focus_word:stack_1764` correlation=no_clear_correlation

## Multiple Families Across Runs

- `region_crc:resume_context_2200` families=6 correlation=no_clear_correlation
  - `0xC1D0E068/0xC1D0E068/0xC1D0E068` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0xB19D04EF/0xB19D04EF/0xB19D04EF` -> BE300Probe_v4_cold_21463.txt
  - `0x7D7361B8/0x7D7361B8/0x7D7361B8` -> BE300Probe_v4_cold_21656.txt
  - `0xDD851290/0xDD851290/0xDD851290` -> BE300Probe_v4_cold_22664.txt
  - `0xD84146E5/0xD84146E5/0xD84146E5` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0xE187199B/0xE187199B/0xE187199B` -> BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
- `region_crc:stack_frame_1700` families=12 correlation=per_run_unique
  - `0xE249636F/0xA6E28ECC/0xB81E0C44` -> BE300Probe_v4_cold_20091.txt
  - `0xA47FBC44/0x232507CA/0x3D7DBB33` -> BE300Probe_v4_cold_21463.txt
  - `0xDFB088CD/0x7A69BA41/0xE55CD559` -> BE300Probe_v4_cold_21656.txt
  - `0xDEEA5523/0x1BB7BB79/0x6EE6F9D8` -> BE300Probe_v4_cold_22664.txt
  - `0x92D6ABD8/0x46855385/0x273093E5` -> BE300Probe_v4_cold_26319.txt
  - `0x2C160B14/0x2C1B0588/0xD5B4B135` -> BE300Probe_v4_unknown_112248.txt
  - `0x0D4EC77F/0x20A44D0D/0x0DBB14C9` -> BE300Probe_v4_warm_11583.txt
  - `0x2B181EA2/0x3CD78822/0x06324598` -> BE300Probe_v4_warm_11956.txt
  - `0x29E14C77/0xF7743121/0x6A97B98D` -> BE300Probe_v4_warm_14709.txt
  - `0xD9D966A3/0xAC4DBE39/0x222E30B0` -> BE300Probe_v4_warm_15212.txt
  - `0xBD6CA6AA/0x8E4F911A/0x2989D704` -> BE300Probe_v4_warm_81147.txt
  - `0x0DABB6EF/0x08081FF0/0x7958B785` -> BE300Probe_v4_warm_9550.txt
- `region_crc:callback_globals_80679400` families=6 correlation=no_clear_correlation
  - `0x76516A1A/0x76516A1A/0x76516A1A` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x209F41C0/0x209F41C0/0x209F41C0` -> BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x44ABB7F5/0x44ABB7F5/0x44ABB7F5` -> BE300Probe_v4_cold_21656.txt
  - `0xDF7C2C27/0xDF7C2C27/0xDF7C2C27` -> BE300Probe_v4_warm_11956.txt
  - `0xD9FBE401/0xD9FBE401/0xD9FBE401` -> BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x33D26683/0x33D26683/0x33D26683` -> BE300Probe_v4_warm_9550.txt
- `region_crc:vrc4173_base` families=12 correlation=per_run_unique
  - `0x2AD41E27/0x9DBE9DF4/0x87CEEC00` -> BE300Probe_v4_cold_20091.txt
  - `0x5D4013F2/0x9618388E/0xC9469D72` -> BE300Probe_v4_cold_21463.txt
  - `0xBE9D5CA7/0x7FDAA888/0x94135993` -> BE300Probe_v4_cold_21656.txt
  - `0x4221B52C/0x421F368F/0x3D5FCDE0` -> BE300Probe_v4_cold_22664.txt
  - `0x6D431D0F/0x08BFE493/0x91E69504` -> BE300Probe_v4_cold_26319.txt
  - `0x1CC0B976/0xB640900F/0x297A506E` -> BE300Probe_v4_unknown_112248.txt
  - `0x316D13D1/0x425BC2D3/0xD9324397` -> BE300Probe_v4_warm_11583.txt
  - `0x4F499202/0x43DDAB97/0xE15ADCE8` -> BE300Probe_v4_warm_11956.txt
  - `0x4CEABD0F/0x1FD8690D/0xCA6D83DB` -> BE300Probe_v4_warm_14709.txt
  - `0x82CA52C4/0x89403AEA/0x6FCFB091` -> BE300Probe_v4_warm_15212.txt
  - `0x92E24043/0x8B926D28/0x0F0F2511` -> BE300Probe_v4_warm_81147.txt
  - `0x7BA7E207/0x2ED7BDDD/0xC05E7D7E` -> BE300Probe_v4_warm_9550.txt
- `region_crc:vr4131_icu_pmu_window` families=12 correlation=per_run_unique
  - `0xBA436205/0xEA094B52/0xF056A5E5` -> BE300Probe_v4_cold_20091.txt
  - `0x0E030C94/0x6F667CF2/0xB3698A41` -> BE300Probe_v4_cold_21463.txt
  - `0xB05B8998/0x7BD6A00D/0xE4E23F0E` -> BE300Probe_v4_cold_21656.txt
  - `0xD754B513/0x38375F1F/0x1A0D9F29` -> BE300Probe_v4_cold_22664.txt
  - `0x07F38927/0xA96DE276/0x703F3808` -> BE300Probe_v4_cold_26319.txt
  - `0xBD534B30/0x9BAFBD4D/0x509BB7F3` -> BE300Probe_v4_unknown_112248.txt
  - `0xC6CFFC20/0x2B575506/0x49021CBD` -> BE300Probe_v4_warm_11583.txt
  - `0xF50E8770/0x50A55CCB/0x95F1E702` -> BE300Probe_v4_warm_11956.txt
  - `0x2DA1F54D/0xC8D9CBC1/0x660C65E3` -> BE300Probe_v4_warm_14709.txt
  - `0xE981C851/0x583DC6AD/0xB1EE1D62` -> BE300Probe_v4_warm_15212.txt
  - `0xB9CDC8C1/0x96289E2D/0x2C1ACA9F` -> BE300Probe_v4_warm_81147.txt
  - `0x47EFDB7F/0xD37B183B/0xA0839362` -> BE300Probe_v4_warm_9550.txt
- `focus_word:nand_sideband_c38` families=12 correlation=per_run_unique
  - `0x0000004D/0x000000E5/0x000000BD` -> BE300Probe_v4_cold_20091.txt
  - `0x00000097/0x00000020/0x000000D7` -> BE300Probe_v4_cold_21463.txt
  - `0x00000094/0x00000054/0x0000008E` -> BE300Probe_v4_cold_21656.txt
  - `0x000000CE/0x0000004E/0x0000009A` -> BE300Probe_v4_cold_22664.txt
  - `0x00000037/0x000000BB/0x00000072` -> BE300Probe_v4_cold_26319.txt
  - `0x00000035/0x00000077/0x00000051` -> BE300Probe_v4_unknown_112248.txt
  - `0x00000057/0x0000009D/0x0000003F` -> BE300Probe_v4_warm_11583.txt
  - `0x000000ED/0x00000095/0x000000D8` -> BE300Probe_v4_warm_11956.txt
  - `0x000000EA/0x0000003D/0x00000061` -> BE300Probe_v4_warm_14709.txt
  - `0x0000005F/0x0000000C/0x00000098` -> BE300Probe_v4_warm_15212.txt
  - `0x00000011/0x00000086/0x000000D6` -> BE300Probe_v4_warm_81147.txt
  - `0x00000063/0x000000CC/0x00000047` -> BE300Probe_v4_warm_9550.txt
- `focus_word:pmu_0100` families=12 correlation=per_run_unique
  - `0x14046A80/0x1406095F/0x140667F2` -> BE300Probe_v4_cold_20091.txt
  - `0x14053363/0x1406D3BA/0x14073161` -> BE300Probe_v4_cold_21463.txt
  - `0x1404DC6E/0x14069816/0x1406FD1B` -> BE300Probe_v4_cold_21656.txt
  - `0x14051DC9/0x1406BC93/0x14071B59` -> BE300Probe_v4_cold_22664.txt
  - `0x1404E281/0x140684C4/0x1406E65A` -> BE300Probe_v4_cold_26319.txt
  - `0x9C354C70/0x9C36F008/0x9C374F59` -> BE300Probe_v4_unknown_112248.txt
  - `0x9C5653B9/0x9C57F43E/0x9C585252` -> BE300Probe_v4_warm_11583.txt
  - `0x14290814/0x142AA7DF/0x142B06E2` -> BE300Probe_v4_warm_11956.txt
  - `0x1462A5D0/0x146444CA/0x1464A525` -> BE300Probe_v4_warm_14709.txt
  - `0x1445F3BC/0x1447942A/0x1447F328` -> BE300Probe_v4_warm_15212.txt
  - `0x14202D93/0x14220C5D/0x14227733` -> BE300Probe_v4_warm_81147.txt
  - `0x14313960/0x1432D852/0x143339B0` -> BE300Probe_v4_warm_9550.txt
- `focus_word:ctx_2220` families=5 correlation=no_clear_correlation
  - `0x0081F124/0x0081F124/0x0081F124` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x0081C024/0x0081C024/0x0081C024` -> BE300Probe_v4_cold_21463.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_21656.txt, BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
  - `0x00814004/0x00814004/0x00814004` -> BE300Probe_v4_cold_22664.txt
  - `0x00814000/0x00814000/0x00814000` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
- `focus_word:ctx_2228` families=4 correlation=no_clear_correlation
  - `0x1090C760/0x1090C760/0x1090C760` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x10908040/0x10908040/0x10908040` -> BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_22664.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_21656.txt, BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
  - `0x00808000/0x00808000/0x00808000` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
- `focus_word:ctx_2274` families=5 correlation=no_clear_correlation
  - `0x00016894/0x00016894/0x00016894` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x00016890/0x00016890/0x00016890` -> BE300Probe_v4_cold_21463.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_21656.txt, BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt
  - `0x00012090/0x00012090/0x00012090` -> BE300Probe_v4_cold_22664.txt
  - `0x00002010/0x00002010/0x00002010` -> BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
- `focus_word:stack_1760` families=2 correlation=no_clear_correlation
  - `0x00000001/0x00000001/0x00000001` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_21656.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0x00000001/0xBDBDBDBD/0x00000001` -> BE300Probe_v4_warm_11956.txt
- `focus_word:stack_1764` families=2 correlation=no_clear_correlation
  - `0x00008000/0x00008000/0x00008000` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_21656.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_unknown_112248.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt, BE300Probe_v4_warm_81147.txt, BE300Probe_v4_warm_9550.txt
  - `0x00008000/0xFFFFD788/0x00008000` -> BE300Probe_v4_warm_11956.txt
- `focus_word:stack_1788` families=10 correlation=tag_disjoint
  - `0x80E64000/0x80E64000/0x80E64000` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_21656.txt
  - `0x80DFA6B8/0x80DFA6B8/0x80DFA6B8` -> BE300Probe_v4_cold_21463.txt
  - `0x80DFCC8C/0x80DFCC8C/0x80DFCC8C` -> BE300Probe_v4_cold_22664.txt
  - `0x80DFB8FC/0x80DFB8FC/0x80DFB8FC` -> BE300Probe_v4_cold_26319.txt
  - `0x80D05AD8/0x80D05AD8/0x80D05AD8` -> BE300Probe_v4_unknown_112248.txt
  - `0x80DF1400/0x80DF1400/0x80DF1400` -> BE300Probe_v4_warm_11583.txt
  - `0x80DFF758/0x80DFF758/0x80DFF758` -> BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_15212.txt
  - `0x80DFF7A0/0x80DFF7A0/0x80DFF7A0` -> BE300Probe_v4_warm_14709.txt
  - `0x80E0B764/0x80E0B764/0x80E0B764` -> BE300Probe_v4_warm_81147.txt
  - `0x80F6CA20/0x80F6CA20/0x80F6CA20` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g94EC` families=5 correlation=no_clear_correlation
  - `0x0064002E/0x0064002E/0x0064002E` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x0000002F/0x0000002F/0x0000002F` -> BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x00630065/0x00630065/0x00630065` -> BE300Probe_v4_cold_21656.txt
  - `0x006E0065/0x006E0065/0x006E0065` -> BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x006C0061/0x006C0061/0x006C0061` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g94F0` families=5 correlation=no_clear_correlation
  - `0x006C006C/0x006C006C/0x006C006C` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x42000074/0x42000074/0x42000074` -> BE300Probe_v4_cold_21656.txt
  - `0x00610064/0x00610064/0x00610064` -> BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x006E0065/0x006E0065/0x006E0065` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g9508` families=4 correlation=no_clear_correlation
  - `0x00000000/0x00000000/0x00000000` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt, BE300Probe_v4_warm_81147.txt
  - `0x002D0030/0x002D0030/0x002D0030` -> BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x00000573/0x00000573/0x00000573` -> BE300Probe_v4_cold_21656.txt
  - `0xC0000014/0xC0000014/0xC0000014` -> BE300Probe_v4_warm_9550.txt
- `focus_word:g9510` families=5 correlation=no_clear_correlation
  - `0x004E0006/0x004E0006/0x004E0006` -> BE300Probe_v4_cold_20091.txt, BE300Probe_v4_cold_26319.txt, BE300Probe_v4_warm_11583.txt, BE300Probe_v4_warm_81147.txt
  - `0x00360036/0x00360036/0x00360036` -> BE300Probe_v4_cold_21463.txt, BE300Probe_v4_cold_22664.txt, BE300Probe_v4_unknown_112248.txt
  - `0x00000030/0x00000030/0x00000030` -> BE300Probe_v4_cold_21656.txt
  - `0x000004C2/0x000004C2/0x000004C2` -> BE300Probe_v4_warm_11956.txt, BE300Probe_v4_warm_14709.txt, BE300Probe_v4_warm_15212.txt
  - `0x000004C7/0x000004C7/0x000004C7` -> BE300Probe_v4_warm_9550.txt

## Seed Eligibility Table

| Focus Word / Region | v4 Status | Families | Eligibility |
|---------------------|-----------|----------|-------------|
| nand_c30 | stable all 12 runs | 1 | stable-seed |
| nand_c34 | stable all 12 runs | 1 | stable-seed |
| nand_c48 | stable all 12 runs | 1 | stable-seed |
| nand_c4c | stable all 12 runs | 1 | stable-seed |
| icu_0080 | stable all 12 runs | 1 | stable-seed (deferred: new ICU seed not added in this pass) |
| stack_179C | stable all 12 runs | 1 | stable-seed (not currently seeded) |
| stack_17B4 | stable all 12 runs | 1 | stable-seed (not currently seeded) |
| vr4131_bcu_window | stable all 12 runs | 1 | stable-seed |
| callback_table_51600 | stable all 12 runs | 1 | stable-seed (not currently seeded) |
| resume_context_2200 | 6 families, no_clear_correlation | 6 | post-boot-only |
| stack_frame_1700 | per_run_unique | 12 | post-boot-only |
| callback_globals_80679400 | 6 families, no_clear_correlation | 6 | post-boot-only |
| vrc4173_base | per_run_unique | 12 | post-boot-only |
| vr4131_icu_pmu_window | per_run_unique | 12 | post-boot-only |
| nand_sideband_c38 | per_run_unique | 12 | phase-varying |
| pmu_0100 | per_run_unique | 12 | phase-varying |
| stack_1760 | 2 families, no_clear_correlation (1 warm settle anomaly) | 2 | post-boot-only |
| stack_1764 | 2 families, no_clear_correlation (1 warm settle anomaly) | 2 | post-boot-only |
| stack_1788 | 10 families, tag_disjoint | 10 | post-boot-only (runtime pointer) |
| ctx_2220 | 5 families, no_clear_correlation | 5 | post-boot-only |
| ctx_2228 | 4 families, no_clear_correlation | 4 | post-boot-only |
| ctx_2274 | 5 families, no_clear_correlation | 5 | post-boot-only |
| g94EC-g9510 | 4-5 families, no_clear_correlation | 4-5 | post-boot-only (UTF-16 strings) |

## De-Speculation Summary

Removed seed families (commit: warm-state de-speculation pass):

| Seed Family | PA Range | v4 Evidence | Action |
|-------------|----------|-------------|--------|
| KData 256 words | 0x2000-0x23FF | resume_context_2200: 6 families across 12 runs | `seed_wince_kdata()` gutted to log-only stub |
| Stack frame reconstruction | 0x1760/0x1764 | Depends on multi-family ctx_2200; stack_1760/1764: 2 families (1 warm settle anomaly disqualifies) | Removed with kdata stub |
| Probe seed words (1754 entries) | scattered SDRAM | Post-boot runtime page, no boot-time grounding | `seed_wince_probe_boot_safe()` gutted to log-only stub |
| Deferred seed mechanism | scattered SDRAM | Same unsupported table | `seed_wince_probe_deferred()` gutted to log-only stub |
| VRC4173 non-NAND seeds (~130 entries) | 0x0A000000+ | vrc4173_base CRC per-run unique (12/12) | Reduced to 4 NAND C-window + 0x1B10 |
| VRC4173 0x01-fills (445 words) | 0x0A001000+ | vrc4173_base CRC per-run unique | Removed entirely |
| VRC4173 nand_sideband_c38 seed | 0x0A000C38 | per_run_unique (12/12) | Kept as state machine init, not survey-derived |
| Internal I/O: BCU (0x000-0x01F) | 0x0F000000-0x0F00001F | Dispatched to bcu_read(), dead in fallback | Removed from fallback seeds |
| Internal I/O: CMU (0x060) | 0x0F000060 | Dispatched to cmu_read(), dead in fallback | Removed from fallback seeds |
| Internal I/O: ICU (0x080-0x0AC) | 0x0F000080-0x0F0000AC | Dispatched to icu_read(), dead in fallback | Removed from fallback seeds |
| Internal I/O: RTC (0x100-0x13F) | 0x0F000100-0x0F00013F | Dispatched to rtc_read(); pmu_0100 per-run unique | Removed from fallback seeds |
| Internal I/O: GPIO (0x140-0x158) | 0x0F000140-0x0F000158 | Dispatched to gpio_read(), dead in fallback | Removed from fallback seeds |

Retained seeds:

| Seed | Location | v4 Evidence |
|------|----------|-------------|
| Boot ROM window (1024 words) | `seed_wince_bootrom_window()` | ROM alias, always present |
| Exception vectors (PA 0x0000-0x0190) | `seed_wince_exception_vectors()` | WinCE ISR framework |
| BCU registers | `apply_wince_bcu_warm_profile()` | vr4131_bcu_window CRC stable all 12 runs |
| VRC4173 NAND C30/C34/C48/C4C | `vrc4173_seed_core_dump_once()` | Stable all 12 runs |
| VRC4173 0x01B10 | `vrc4173_seed_core_dump_once()` | NK reads & branches on bit 6 |
| CP0 Status warm value | `apply_wince_warm_profile()` | Functional requirement |
| Internal I/O fallback (unmodeled) | `seed_internal_io_fallback()` | 0x020-0x034, 0x040, 0x064-0x07C, 0x0C0-0x0D4, 0x180-0x188, 0x1E0-0x1EC |

