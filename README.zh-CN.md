# Windows 涓婄殑 NVMe-oF over RDMA锛堣嚜鐮?NetworkDirect 鏍堬級

**涓枃** | [English](README.md)

杩欎釜宸ョ▼鐢?*鎴戜滑鑷繁鐨?NetworkDirect/NDSPI 浠ｇ爜**鍦?Windows 涓婂疄鐜?NVMe-oF/RDMA
浼犺緭锛歠abrics 鍛戒护銆?4 瀛楄妭 capsule銆乲eyed SGL/STag銆丷DMA Read/Write銆?鍐呭瓨娉ㄥ唽銆佹瘡涓?I/O 闃熷垪鐙珛 queue pair锛?*initiator 涓?target 涓ょ閮芥槸鎴戜滑鐨勪唬鐮?*锛?娌℃湁浣跨敤浠讳綍绗笁鏂?NVMe-oF 瀹炵幇銆?
鍏堣缁撹鍐嶈浠ｇ爜锛?*[DESIGN.md](DESIGN.md)** 鏄璁′笌瀹炴祴璁板綍锛埪? 鏄€愭潯韪╁潙鍙诧級锛?**[INTEROP_F5.md](INTEROP_F5.md)** 鏄?濡備綍鐢ㄤ竴鍙?Linux 鏈哄櫒鍋氱嫭绔嬪绔?鐨勬搷浣滄墜鍐屻€?
---

## 鐩綍

| 璺緞 | 鏄粈涔?|
|---|---|
| `src/nvmeof_wire.h` | 绾夸笂鏍煎紡锛歝apsule/CQE/SGL/Identify/鐘舵€佺爜锛屾瘡涓竷灞€閮芥湁 `NVMEOF_STATIC_ASSERT` 閽変綇 |
| `src/nvmeof_rdma.h` | 浼犺緭灞傦細`Device` / `Queue` / `acceptChecked()`锛堟寜 Linux 鐨勮鍒欐牎楠?Connect 绉佹湁鏁版嵁锛?|
| `src/f1_bringup.cpp` | F1 杩炴帴涓?Identify |
| `src/f3_io.cpp` | F3 璇诲啓姝ｇ‘鎬?+ 閿欒璺緞锛堝惈 0x4f invalidate 瀛愮被鍨嬶級 |
| `src/f4_pipeline.cpp` | F4 娴佹按绾垮悶鍚愶紙8 鏉″湪椋烇級 |
| `src/f5_interop.cpp` | **F5 浜掓搷浣?*锛歚-initiator` 鎵撲换鎰忓绔紝`-target` 缁欑湡瀹?host 鐢紙32 妲?receive ring锛岃鐩栧畠瀹ｇО鐨勭獥鍙ｏ級 |
| `src/f6_lifecycle.cpp` | F6 host 瀹屾暣搴忓垪 + 鐩爣渚х敓鍛藉懆鏈燂紙31 鏉℃柇瑷€锛?|
| `src/f7_faults.cpp` | F7 鏁呴殰娉ㄥ叆锛堝绔秷澶便€佸弽鍚戞秷澶憋級 |
| `src/nvmeof_auth.h` | DH-HMAC-CHAP 鐨勫瘑鐮佸鍘熻锛欳NG 鐨?SHA/HMAC + 鑷爺瀹氶暱 Montgomery 妯″箓锛坄nvmeof_bignum.h`锛? RFC 7919 ffdhe 缇わ紙`nvmeof_dhgroups.h`锛?|
| `src/nvmeof_dhchap.h` | DH-HMAC-CHAP 鍗忚锛氬瘑閽ヨВ鏋愶紙DHHC-1 + CRC32锛夈€乣Kt` 鍙樻崲銆乼arget 鍗婅竟銆乭ost 鍗婅竟銆佺幆鍥炶嚜妫€ |
| `src/wire_selftest.c` | 瀛楄妭绾?golden 鑷锛孋 涓?C++ 鍙屼唤缂栬瘧 |
| `src/xref_constants.py` | 甯搁噺涓庝袱浠?Linux 鍙傝€冨ご鏂囦欢**閫愬€兼瘮瀵?*锛?7 瀵癸紝鍚?DH-HMAC-CHAP 鐨?21 涓級 |
| `src/run_all.ps1` | 涓€娆¤窇瀹屽叏閮?10 濂楀苟缁欑粨璁鸿〃锛堢害 220 绉掞級 |
| `src/interop_link.ps1` | 浜掓搷浣滈摼璺細妗ユ帴 + 鎼?IP/璺敱 + 闄?MTU锛屽け璐ヨ嚜鍔ㄥ洖婊?|
| `src/f5_session.ps1` | **浜掓搷浣滄暣鍦?*锛氶摼璺?鈫?瀵圭 鈫?鏂瑰悜 A 鈫?discovery 鈫?鏂瑰悜 B 鈫?涓€浠芥姤鍛婃枃浠讹紱`-Auth` 鎹㈡垚甯﹁璇佺殑鐗堟湰 |
| `src/run_f5_auth.ps1` | DH-HMAC-CHAP 鍏釜鐢ㄤ緥锛堟湰鏈轰袱鍙ｇ洿杩烇紝涓嶉渶瑕?Linux锛?|
| `src/nvmeof_iscsi.h` | **鐢ㄦ埛鎬?iSCSI target**锛氫笁娈靛紡 login銆丼endTargets銆丯OP/Logout/TaskMgmt銆丼CSI 鍛戒护闆嗭紙INQUIRY/VPD銆丮ODE SENSE銆丷EAD CAPACITY銆丷EPORT LUNS銆丷EAD/WRITE(10/16) 璧?R2T銆丼YNCHRONIZE CACHE锛夈€傛瘡杩炴帴涓€涓璞′竴涓嚎绋嬶紝backend锛堜竴鏉￠槦鍒楀锛夌敤浜掓枼閲忎覆琛?|
| `src/tools_login_probe.ps1` | 鎶?Windows 鐪熷疄鍙戝嚭鐨?iSCSI login **閫愬瓧鑺傞噸鏀?*缁欎换鎰?target锛堝仛 LIO 鍙傝€冨疄鐜扮殑 A/B 瀵规瘮鐢級 |
| `src/mount_nvmeof.ps1` | 鍛藉悕绌洪棿 鈫?fixed VHD 鈫?鐩樼锛屽嵏杞芥椂鍥炴帹 |
| `EVIDENCE-1TB.md` | 1 TB 鐪熺洏鎸傚埌 Windows 鐨勫叏閮ㄥ疄娴嬭緭鍑猴紙鍚媶闄よ褰曪級 |
| `ref/linux_nvme.h`銆乣ref/linux_nvme_rdma.h` | 鍙傝€冨壇鏈紝渚涗笂闈㈠姣斾笌鏌ヨ瘉 |
| `linux/` | 瀵圭鑴氭湰锛歚nvmet_setup.sh`锛堝惈 rxe 杞欢 RoCE 涓庡彲閫?`AUTH_KEY=` 璁よ瘉锛夈€乣f5_linux_up.sh`銆乣nvmet_teardown.sh`銆乣f5_dirb_check.sh`锛堟柟鍚?B锛夈€乣f5_dirb_auth.sh`锛堟柟鍚?B + 璁よ瘉锛夈€乣f5_dirb_demo.sh`锛堝鎵弿 + 鎸佷箙鍖栨紨绀猴級銆乣f5_nvmet_ref*.sh`锛堟妸 nvmet 褰撹鏍奸€愯閲忓弬鑰冪瓟妗堬級銆乣f5_dsm_check.sh`锛圖SM 鐨?AD 浣嶅埌搴曞湪鍝級 |

## 鍓嶇疆鏉′欢锛堣鑷繁缂栬瘧鐨勮瘽锛?
1. **Visual Studio**锛堝惈 C++ 妗岄潰宸ヤ綔璐熻浇锛岀敤瀹冪殑 `VsDevCmd.bat` 鎻愪緵 `cl.exe`锛夈€?2. **NetworkDirect / NDSPI 鐨勫ご鏂囦欢涓庡簱**锛歚ndspi.h`銆乣ndutil.h`/`ndutil.lib`銆?*鏈粨搴撲笉鍚繖浜?*
   锛堝畠浠笉鏄湰宸ョ▼鐨勪唬鐮侊紝鏉ヨ嚜 WDK/Windows SDK 鐨?NetworkDirect 閮ㄥ垎锛屾垨缃戝崱鍘傚晢鐨?   ND 鎻愪緵鑰呭畨瑁呭寘锛涘疄娴嬬幆澧冩槸 HP/Mellanox ConnectX-3 Pro + WinOF 鐨?ND 鎻愪緵鑰咃級銆?   缂栬瘧鏃惰繕闇€瑕佸巶鍟嗙殑 NDv2 澶达紙Mellanox 鐨?`鈥MLNX_VPI\IB\SDK\inc\ndv2`锛夛紝鏈夊垯鍔犲叆鍖呭惈璺緞銆?3. 涓変釜璺緞閫氳繃**鐜鍙橀噺**瑕嗙洊锛屼笉璁惧氨鐢ㄦ湰鏈哄疄娴嬬殑榛樿鍊硷紝**涓嶉渶瑕佹敼鑴氭湰**锛?
   | 鐜鍙橀噺 | 鍚箟 | 涓嶈鏃剁殑榛樿鍊?|
   |---|---|---|
   | `ND_VS_DIR` | Visual Studio 瀹夎鐩綍 | `F:\Microsoft Visual Studio\18\Community` |
   | `ND_NDUTIL_INC` | NetworkDirect 澶存枃浠剁洰褰?| `D:\rdma\NetworkDirect\src\ndutil` |
   | `ND_NDUTIL_LIB` | `ndutil.lib` 鎵€鍦ㄧ洰褰?| `D:\rdma\NetworkDirect\src\x64\Release` |
   | `ND_MLNX_INC` | 鍘傚晢 NDv2 澶寸洰褰曪紙鍙€夛級 | `C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2` |

   鑴氭湰鑷繁鎵€鍦ㄧ洰褰曚竴寰嬬敤 `$PSScriptRoot` 鎺ㄥ锛屾墍浠ヤ粨搴撴斁鍦ㄥ摢閲岄兘鑳借窇锛?
   ```powershell
   $env:ND_VS_DIR     = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
   $env:ND_NDUTIL_INC = 'C:\ndsdk\src\ndutil'
   $env:ND_NDUTIL_LIB = 'C:\ndsdk\src\x64\Release'
   cd <浠撳簱>\src ; .\run_wire.ps1        # 鍏堣窇杩欎釜锛氫笉闇€瑕佺綉鍗?   ```

4. `ref/` 涓嬩袱浠芥槸 Linux 鍐呮牳鐨勫弬鑰冨ご鏂囦欢锛坄linux_nvme.h` / `linux_nvme_rdma.h`锛?   GPL-2.0 鐨?UAPI/鍐呮牳澶达級锛?*鍙綔瀵圭収闃呰锛屼笉鍙備笌缂栬瘧**銆?
## 璺戣捣鏉?
涓€娆¤窇鍏ㄩ儴锛堢害 6 鍒嗛挓锛屾寜椤哄簭鎵ц銆佷簰鐩镐笉骞叉壈锛夛細

```powershell
cd <浠撳簱>\src
.\run_all.ps1
```

鍗曠嫭璺戞煇涓€濂楋細`run_xref.ps1`銆乣run_wire.ps1`銆乣run_f1.ps1`銆乣run_f3.ps1`銆?`run_f4.ps1`銆乣run_f5.ps1`銆乣run_f5_auth.ps1`銆乣run_f6.ps1`銆乣run_f7.ps1`銆?
姣忓鐨勮鍒欓兘涓€鏍凤細**鍒犳帀鏃?exe 鈫?鐪嬬紪璇戦€€鍑虹爜 鈫?姣斿婧愮爜涓庡ご鏂囦欢鏃堕棿鎴?*锛?缁濅笉杩愯"鐪嬭捣鏉ヨ繕鍦?鐨勬棫浜岃繘鍒讹紙杩欐潯鏄粠涓€娆＄湡瀹炰簨鏁呴噷鏉ョ殑锛岃 DESIGN 搂8.6锛夈€?
## 瑕佹帴鐪熷疄 host 鏃剁敤鍝釜浜岃繘鍒?
| 鍦烘櫙 | 鐢ㄤ粈涔?|
|---|---|
| Linux `nvme-cli` / 浠讳綍澶栨潵 host 鎵撴垜浠?| **`f5_interop.exe -target <ip> <port>`** |
| 鎴戜滑鐨?host 鎵?Linux `nvmet` 鎴栦换浣曞绔?| `f5_interop.exe -initiator <serverIp> <port> <localIp> [-subnqn <nqn>]` |
| 鑷祴锛堜袱绔兘鏄垜浠級 | 浠绘剰 `run_f*.ps1` |

**F6 鐨?target 鏄敓鍛藉懆鏈熸祴璇曟浛韬紝涓嶆槸浜掓搷浣?target**锛氬畠鐨?host 鏄弗鏍间竴闂竴绛旓紝
鎵€浠ュ畠鍙寕涓€涓?Receive锛岄亣鍒颁細娴佹按鐨勭湡瀹?host 浼氫涪 capsule锛圖ESIGN 搂8.41锛夈€?F5 鐨?target 鏈?8 妲?receive ring 涓庡欢杩熷畬鎴愶紝鑳芥墰浣忛槦鍒楁繁搴?32 鐨?host銆?
## 褰撳墠鐘舵€?
| 楠屾敹椤?| 鐘舵€?|
|---|---|
| 1 wire 鑷 | 鉁?`run_wire.ps1` |
| 2 涓ょ Identify 涓€鑷?| 鉁?F1 / F6 |
| 3 鍐欏悗璇婚€愬瓧鑺備竴鑷?| 鉁?F3 |
| 4 閿欒璺緞鏈夌晫锛堝惈瀵圭娑堝け锛屽弻鍚戯級 | 鉁?F3 / F6 / F7 |
| 5 **鎴戜滑鐨?host 鈫?Linux `nvmet`** | 鉁?**璺戦€氫簡**锛歚initiator failures: 0`锛?6 椤规鏌ワ級锛屽惈閫愬瓧鑺備竴鑷寸殑 WRITE/READ銆?2 鏉″湪椋炵殑娴佹按绾匡紝浠ュ強 **4 鏉?I/O 闃熷垪鍚勮嚜鍚屾椂璺戜竴鏉″懡浠?*锛圖ESIGN 搂8.46銆伮?.49锛?|
| 6 鍚炲悙 vs 瑁?RDMA 鍩虹嚎 | 鉁?绾?1.17 GB/s 鈮?2.53 GB/s 鐨?**48%**锛圖ESIGN 搂8.16锛?|
| 7 **鎴戜滑鐨?target 鈫?Linux `nvme-cli`** | 鉁?**璺戦€氫簡**锛歚nvme connect` rc=0锛宍nvme list` 鍑虹幇 `NDVMEOF0000000000001`锛?28 鍧楀啓鍏モ啋flush鈫掕鍥?**`cmp` 閫愬瓧鑺傜浉鍚?*锛涗富鏈哄缓 **8 鏉?I/O 闃熷垪**銆佸懡浠ゅ垎鏁ｅ湪 **6 鏉?*涓婏紙DESIGN 搂8.46銆伮?.49锛?|
| 8 **DH-HMAC-CHAP 鍦ㄥ甫鍐呰璇?* | 鉁?**涓や釜鏂瑰悜閮借窇閫氫簡锛圠inux 瀵圭瀹炴祴锛?*锛氱湡 Linux 涓绘満鐢?`nvme connect -S <key>` 璁よ瘉鍒版垜浠殑 target锛堝唴鏍告棩蹇?`qid 0: authenticated with hash hmac(sha256) dhgroup ffdhe2048`锛屽崟鍚戜笌**鍙屽悜**閮介€氳繃骞舵惉杩愪簡鏁版嵁锛?*閿欏瘑閽ヨ鎷?*锛宍authRefused=0`锛夛紱鍙嶅悜涔熶竴鏍封€斺€旀垜浠殑 host 璁よ瘉鍒?*瑕佹眰璁よ瘉鐨?nvmet**锛宍Success2 sent (the controller's own response verified)`锛屽嵆鐪?ffdhe2048 DH + 鍙屽悜锛圖ESIGN 搂8.51銆伮?.52锛?|

绗?5銆? 椤规槸鍞竴鑳藉彂鐜?鎴戜滑涓ょ涓€璧峰啓閿?鐨勬祴璇曗€斺€旂涓€娆＄湡璺戯紝
瀹冧滑涓€鍏辨姄鍑?**11 涓己闄?*锛屽叾涓?7 涓槸鎴戜滑涓ょ瀵瑰悓涓€瀛楁鐨勭悊瑙ｄ笌瑙勮寖涓嶄竴鑷达紝
鑰屾鍓?8/8 鑷鍏ㄧ豢锛圖ESIGN 搂8.46锛夈€?*鑷璇佹槑鍐呴儴涓€鑷达紝涓嶈瘉鏄庢纭€?*

**杩欎袱椤圭幇鍦ㄦ槸涓€鏉″懡浠?*锛堝疄娴嬶細鍙屽悜閮借繃锛屼箣鍚?`run_all.ps1` 9/9銆?37 绉掞級锛?
```powershell
.\f5_session.ps1 -LaptopIp <瀵圭> -LaptopUser <鐢ㄦ埛>        # 閾捐矾 鈫?瀵圭 鈫?鏂瑰悜 A 鈫?discovery 鈫?鏂瑰悜 B
.\f5_session.ps1 -Auth -LaptopIp <瀵圭> -LaptopUser <鐢ㄦ埛>  # 鍚屼竴鏉￠摼璺紝鏂瑰悜 B 甯?DH-HMAC-CHAP
.\f5_session.ps1 -Down                                      # 鎷嗘ˉ + 鍥炶楠岃瘉
```

鍘熷杈撳嚭钀藉湪 `src/f5_session_<鏃堕棿鎴?.txt`銆傛妸鎵嬪伐姝ラ鍥哄寲鎴愯剼鏈殑杩囩▼閲?**鍙堟姄鍑?4 涓己闄?*锛堟柟鍚?A 婕忎紶鏈湴 IP銆佹帰娴嬪绔湴鍧€鐨勬椂鏈哄弽浜嗐€?杩滅▼鍛戒护涓茬┛杩?PowerShell鈫抯sh鈫抌ash 浼氫涪寮曞彿銆佹柟鍚?B 鑴氭湰鑷繁鐨勪笁涓潙锛夛紝
瑙?DESIGN 搂8.47 鈥斺€?鍏朵腑"寮曞彿浼氫涪"閭ｄ竴鏉℃浘鍦ㄥ簭鍒椾腑闂村穿鎺夊苟鍦ㄥ绔暀涓嬫椿鎺у埗鍣ㄣ€?
**澶?I/O 闃熷垪锛埪?.49锛?*锛歵arget 鐪熺殑鎷ユ湁 8 涓?queue pair锛堟瘡鏉￠槦鍒楃嫭绔?capsule ring 涓?in-flight 琛級锛宨nitiator 鎸夊绔巿浜堟暟寤洪槦鍒椼€傚绔瘉鎹細鏂瑰悜 A 鏄?4/4 鏉￠槦鍒楀悇鑷缓杩炲苟
鍚屾椂鍚勮窇涓€鏉″懡浠わ紱鏂瑰悜 B 鏄?Linux 涓绘満寤?8 鏉°€佸懡浠ゅ垎鏁ｅ湪 6 鏉′笂銆傝嚜妫€ F5 榛樿灏辩敤 4 鏉￠槦鍒椼€?
**discovery锛埪?.50锛?*锛歚nvme discover` 鑳藉垪鍑烘垜浠紙瀵圭瀹炴祴 `DISC: the discovery log names
nqn.2024-01.local.rdma:windows-nd`锛屾湰鏈?target `discLogReads=3`銆乣controllers=2`锛夛紝
鎴戜滑鑷繁鐨?host 涔熻兘鐢?`-discover` 璇?Linux nvmet 鐨勫彂鐜版棩蹇椼€俤iscovery 涓?I/O 鏄?*涓や釜
鎺у埗鍣?*锛屾墍浠?target 鐢?`-serve N` 杩炵画鏈嶅姟锛堥粯璁?1锛屼繚鎸?F7 鐨?涓绘満娑堝け鍗抽€€鍑?鏂█锛夈€?
**DH-HMAC-CHAP锛埪?.51锛?*锛歚-authkey <DHHC-1:..>` 璁?target 瑕佹眰璁よ瘉锛圕onnect 缁撴灉缃?ATR bit 17锛夛紝
璁よ瘉涔嬪墠闄?fabrics 涔嬪鐨勫懡浠や竴寰嬪洖 `0x4191`锛沨ost 渚х湅鍒?ATR 灏辫嚜鍔ㄥ畬鎴?Negotiate鈫扖hallenge鈫扲eply鈫扴uccess1锛堟湁鎺у埗鍣ㄥ瘑閽ユ椂鍐嶅姞 Success2锛夈€?瀵嗙爜瀛︽槸鑷爺鐨?Montgomery 妯″箓鈥斺€擶indows 鐨?CNG **瀹炴祴**鎷掔粷鑷畾涔?DH 缇?锛坄STATUS_NOT_SUPPORTED`锛夛紝涔熸嫆缁濆鍏ヤ笉鍖归厤鐨勭閽ワ紙`STATUS_INVALID_PARAMETER`锛夛紝
鑰?ffdhe 缇ゅ弬鏁版槸浠?RFC 7919 鍘熸枃鎶撳彇骞堕€愭潯楠岃瘉鐨勩€俙.\f5_interop.exe -genkey` 鐢熸垚瀵嗛挜锛?`.\run_f5_auth.ps1` 璺戝叚涓敤渚嬶紙鍚?閿欏瘑閽ュ繀椤昏鎷?鍜?鏃犺 ATR 蹇呴』琚?gate 鎷?锛夈€?
**涓€涓湡鑳界敤鐨勫嵎锛埪?.53锛?*锛歚-nsfile F:\x.img` 鎶?namespace 鍙樻垚閭ｄ釜鏂囦欢锛堝嚑浣?= 鏂囦欢澶у皬锛?FLUSH 鎵嶅洖鍐欙級锛屼簬鏄竴鍙?Linux 涓绘満鍐欒繘鏉ョ殑瀛楄妭鍙互鐩存帴鍦?Windows 渚х湅鍒帮細
```powershell
# Windows锛歵arget 鐢ㄦ枃浠跺綋 namespace锛?serve 0 = 涓嶉檺鎺у埗鍣ㄦ暟锛?.\f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof-ns.img
# 瀵圭锛堟ˉ鎺ョ獥鍙ｉ噷锛夛細
~/f5_dirb_demo.sh 192.168.100.2 4420 nqn.2024-01.local.rdma:windows-nd
# 鐒跺悗鍦?Windows 涓婄洿鎺ヨ閭ｄ釜鏂囦欢锛氬亸绉?4096 澶勬槸涓绘満鍐欒繘鏉ョ殑鏂囨湰锛?# LBA 3000 / 3100 澶勬槸 write-zeroes 涓?dsm deallocate 娓呭嚭鏉ョ殑闆躲€?```

鍚屼竴杞噷 `linux/f5_nvmet_ref*.sh` 鎶?nvmet 褰撹鏍硷紝閫愯閲忓嚭鍙傝€冪瓟妗堬紙鏃ュ織椤?LID 绛栫暐銆?甯︽暟鎹紦鍐茬殑 Get Features 涓€寰?`SGL_INVALID_DATA`銆丼et VWC 蹇呴』鎷掋€丏SM 鐨?AD 浣嶅湪 CDW11锛夛紝
骞舵嵁姝や慨鎺変簡涓変釜"鎴戜滑涓ょ鑷祴姘歌繙涓€鑷?鐨勫樊寮?鈥斺€?鍏朵腑鍖呮嫭涓€鏉?*浼氭妸鎺у埗鍣ㄦ寕姝?*鐨勶細
`nvme persistent-event-log` 瑙﹀彂鐨?0 瀛楄妭 RDMA Write 姘镐笉瀹屾垚锛屾妸 admin 闃熷垪鍋滄锛?涓绘満 7.6 绉掑悗 Keep Alive 瓒呮椂骞舵媶閾俱€?
浜掓搷浣滅殑鎿嶄綔姝ラ銆侀摼璺剼鏈笌鍥炴粴瑙?`INTEROP_F5.md`锛涚粨璁轰笌缂洪櫡娓呭崟瑙?DESIGN 搂8.46銆伮?.47銆伮?.49銆伮?.50銆伮?.51銆伮?.53銆伮?.54銆?
**Windows 涓婄殑 NVMe-oF 鐩樼锛埪?.54锛?*锛歚mount_nvmeof.ps1` 璁╂垜浠嚜宸辩殑 initiator 鎵紨
Windows 缂哄け鐨勯偅涓鑹诧紙瀹㈡埛绔?SKU 娌℃湁鍐呯疆 NVMe-oF initiator锛夛細
```powershell
# 浠?Linux nvmet 鍙栧洖 64 MiB 鍛藉悕绌洪棿 -> 鍖呮垚 fixed VHD -> 鎸傛垚 X:
.\mount_nvmeof.ps1
# 涓嶆兂瑕?Linux 鏃讹紝鐢ㄦ垜浠嚜宸辩殑 target 褰撴簮锛?.\f5_interop.exe -target 192.168.100.2 4420 -serve 0 -nsfile F:\nvmeof\target-ns.img
.\mount_nvmeof.ps1 -TargetIp 192.168.100.2 -Subnqn nqn.2024-01.local.rdma:windows-nd
# 鍦?X: 閲屽啓鏂囦欢锛岀劧鍚庡洖鎺紙bytes 璧?NVMe-oF/RDMA 鍥炲埌瀵圭鍛藉悕绌洪棿骞?FLUSH锛夛細
.\mount_nvmeof.ps1 -Unmount
```

瀹冩槸**鐪熺殑 Windows 鍗?*锛圢TFS銆佽祫婧愮鐞嗗櫒鍙銆佸彲璇诲啓锛夛紝浣?*涓嶆槸娲诲姩鍧楄澶?*锛?鍐欏叆瑕佺瓑 `-Unmount` 鎵嶅洖鎺ㄣ€傚疄娴嬩竴鏁村湀瀵瑰緱涓?鈥斺€?Linux 璇诲洖 64 MiB 鐨?sha256 涓庢湰鏈洪暅鍍?閫愬瓧鑺傜浉鍚岋紝閲嶆柊鎸傝浇鍚庢枃浠朵粛鍦ㄣ€? MiB 闅忔満鏂囦欢鏍￠獙鍜屼笉鍙樸€?
**閭ｇ"涓嶆槸娲诲姩鍧楄澶?鐨勯仐鎲撅紝鍦?搂8.55 琚幓鎺変簡**锛氬簳涓嬭繖涓€鏉¤矾缁欏嚭鐨勬槸**鐪熺殑娲诲姩纾佺洏**銆?
**鐢ㄦ埛鎬?iSCSI 妗ワ紙搂8.55锛夛細璁?Windows 鑷甫鐨?initiator 鎶婅繙绔洏褰撹嚜宸辩殑鐩樼敤**

鍐呮牳椹卞姩閭ｄ竴灞傛垜浠笉鍋氾紙瀹㈡埛绔?SKU 娌℃湁 NVMe-oF initiator锛岀敤鎴锋€佷篃娌℃硶鎶婅嚜宸卞杩涘嵎鏍堬級锛?鎵€浠ュ弽杩囨潵鍋氾細鎴戜滑鑷繁瀹炵幇 iSCSI 鐨?*瀵圭**锛學indows 鐢ㄥ畠鍐呮牳閲岀幇鎴愮殑 initiator 杩炰笂鏉ャ€?`src/nvmeof_iscsi.h` 鏄偅涓?target锛屽悗绔洿鎺ヨ皟鍚屼竴杩涚▼銆佸悓涓€鏉￠槦鍒楀涓婄殑 NVMe-oF initiator
鈥斺€旀病鏈夌浜屼釜杩炴帴銆佹病鏈?IPC銆?
```powershell
# 鎴戜滑鐨?NVMe-oF initiator 鎺ュ埌 Linux nvmet锛屽悓鏃舵妸缁撴灉鐢?iSCSI 鏆撮湶缁?Windows
.\f5_interop.exe -initiator 192.168.100.5 4420 192.168.100.2 `
                 -subnqn nqn.2024-01.local.rdma:linux-nvmet -iscsi 3260
# Windows 鑷甫鐨?initiator 鐧诲綍锛堜笉闇€瑕佷换浣曞唴鏍搁┍鍔級
iscsicli AddTargetPortal 127.0.0.1 3260
Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $false
Get-Disk | Where-Object BusType -eq 'iSCSI'      # -> NVMEOF iSCSI-NVMeoF, Online, GPT
```

浠?鐧诲綍灏卞崱姝?鍒?鐩樺嚭鐜?涔嬮棿涓€鍏变慨浜?**7 涓?bug**锛屾牴鍥犳槸**鏀跺埌鐨?PDU 娌℃湁璺宠繃 data segment
鐨?4 瀛楄妭瀵归綈濉厖**锛圧FC 7143 搂11.7 鐨?DataSegmentLength 涓嶅惈濉厖锛夛細discovery 鐨?login 鏂囨湰
姝ｅソ 88 瀛楄妭锛? 鐨勫€嶆暟锛屾棤濉厖锛夛紝**鏅€氫細璇濇槸 127 瀛楄妭**锛屾紡鎺夐偅 1 瀛楄妭涔嬪悗姣忎釜 PDU 閮介敊浣嶏紝
initiator 绗簩涓?Login Request 鐨?opcode 钀借繘浜嗘垜浠殑 flags 瀛楁锛岃璇绘垚 NOP-Out锛?浜庢槸 Windows 涓€鐩寸瓑涓€涓笉浼氭潵鐨?Login Response銆傚叾浣欏叚涓紙鍗曚細璇濅覆琛屻€丮axCmdSN 涓嶅墠杩涖€?鍙戦€佷晶鍚屾牱婕忓～鍏呫€佺煭浼犺緭鎶?residual 0銆佹垜鑷繁鏀瑰绾跨▼鏃舵妸宸叉敞鍐岀紦鍐叉崲鎴愮┖ vector銆?浠ュ強"鏃ュ織鍙鍙戦€佷笉璁版帴鏀?鐨勭洸鍖猴級閫愭潯璁板湪 DESIGN 搂8.55銆?
**1 TB 鐪熺洏锛埪?.56锛夛細Linux 涓婇偅鍧?SSD锛屽彉鎴?Windows 鐨?`E:`**

```
Get-Disk | ? BusType -eq iSCSI
Number FriendlyName        PartitionStyle OperationalStatus SizeGB IsReadOnly
     3 NVMEOF iSCSI-NVMeoF GPT            Online            931.51       True
鍒嗗尯锛?00 MB ESP + 16 MB MSR + 931.2 GB NTFS -> E:锛岄《灞?60 椤癸紝鏃堕棿鎴虫槸鐪熺殑
```

- **鍐呭鏄湡鐨勶紝涓ゆ瀛楄妭绾у鐓?*锛氱粡 `E:` 璇讳竴涓枃浠讹紙`fsutil file queryextents` 鎷?LCN锛?  鍔犲垎鍖哄亸绉荤畻鍑虹粷瀵?LBA 1073520锛変笌 Linux 渚ц８璇诲悓涓€ LBA锛宻ha256 閮芥槸
  `dec615ae鈥?dd397`锛汦SP 寮曞鎵囧尯锛圠BA 40锛変袱渚т篃閮芥槸 `802b1462鈥2452293`銆?- **闆跺啓鍏ワ紝鐢辩洏鑷繁璇佹槑**锛歋MART `Data Units Written` 鎸傝浇鍓嶅悗**閮芥槸 32884752**锛?  鑰?`Data Units Read` 浠?144240893 娑ㄥ埌 144241018銆?47 鏉?CDB 閲屽彧鏈?*涓€娆?*鍐欏皾璇?  锛圵indows 鎯冲啓 NTFS 鑴忎綅锛孡BA 651264锛夛紝琚ˉ鎸夊彧璇绘嫆鎴?CHECK CONDITION銆?- 鍙鏄?*鏍忔潌**涓嶆槸榛樿鍊硷細`-iscsirw` 涓嶅紑銆丮ODE SENSE 鎶?WP銆乄RITE 鍦?SCSI 灞傛嫆缁?  鈥斺€?nvmet 娌℃湁鍙 namespace 灞炴€э紝鍙兘鍦ㄨ繖涓€渚т繚璇併€?- 鏈€鍚庝竴娈佃矾鏄摼璺細`src/interop_link.ps1 -Action Up` 鎶婁竴涓?CX3 鍙ｆˉ杩?LAN 娈碉紝
  璁╁绔殑杞欢 RoCE 鑳?ARP 鍒帮紱鏀跺伐鐢?`-Action Down` + `-Action RestoreMtu` 鎶婄粦瀹氳繕缁欑郴缁熴€?
鍏ㄩ儴瀹炴祴杈撳嚭锛堝惈鎷嗛櫎姝ラ涓庤俯鍒扮殑涓変釜鍧戯級瑙?`EVIDENCE-1TB.md`銆?
## 纭欢鍓嶆彁

- 涓€鍙?Windows 鏈哄櫒 + 涓€鍧楁敮鎸?RoCE 鐨勭綉鍗★紙鏈伐绋嬪疄娴嬶細ConnectX-3 Pro锛?  HP 544+FLR-QSFP锛屽浐浠?2.40.5000锛學inOF 鐨?ND 鎻愪緵鑰咃級銆?- 鑷祴鍙涓€鍧楀弻鍙ｅ崱鎶婁袱鍙ｇ洿杩炲嵆鍙€?- 浜掓搷浣滈渶瑕佷竴涓?*鏈湴** Linux 瀵圭锛堟櫘閫氭満鍣?+ 杞欢 RoCE `rxe` 灏卞锛?  浜戞湇鍔″櫒涓嶈鈥斺€擱oCE 涓嶈繃璺敱锛岃 `INTEROP_F5.md`锛夈€?
## 璁稿彲

**GNU Affero General Public License v3.0 鎴栨洿鏂扮増鏈?*锛坄LICENSE`锛夆€斺€?閫愬瓧瀹樻柟鍘熸枃锛?34,523 瀛楄妭锛宻ha256 `8486a10c4393cee1c25392769ddd3b2d6c242d6ec7928e1414efff7dfb2f07ef`銆?
```
Copyright (C) 2026 Dingtaiqi

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
```

**AGPL 鍏佽鍟嗕笟浣跨敤**锛屽畠绠＄殑鏄?闂簮"锛?
- 鍏徃鍐呴儴浣跨敤銆佹嬁瀹冭禋閽便€佸仛鎴愭湇鍔?鈥斺€?**閮藉彲浠ワ紝鍏嶈垂**銆?- 浠ｄ环鏄洖棣堬細鍒嗗彂鏈伐绋嬫垨娲剧敓浣滃搧锛?*鍖呮嫭閫氳繃缃戠粶鎻愪緵鏈嶅姟**锛夋椂蹇呴』缁欏嚭瀹屾暣瀵瑰簲婧愮爜銆?  绗?13 鏉★紙`LICENSE` L540锛夊氨鏄笓闂ㄧ缃戠粶鏈嶅姟鐨勯偅涓€鏉★紝涔熸槸 AGPL 涓?GPL 鐨勫敮涓€瀹炶川鍖哄埆
  鈥斺€?瀵逛粯"鎷垮紑婧愪唬鐮佸仛闂簮浜戞湇鍔?闈犵殑灏辨槸瀹冦€?- 纭疄闇€瑕侀棴婧愶紙宓岃繘闂簮浜у搧銆佸仛闂簮 SaaS锛夌殑鍏徃锛屽彲浠ヨ蛋 `COMMERCIAL.md` 鐨勫晢涓氭巿鏉冿紙鍙屾巿鏉冿級銆?
涓や釜鍏煎鎬у潙锛?
1. `ref/` 涓嬩袱浠?Linux 鍐呮牳澶达紙`linux_nvme.h`銆乣linux_nvme_rdma.h`锛夋槸 **GPL-2.0**锛?   **鍙綔瀵圭収闃呰銆佷笉鍙備笌缂栬瘧**銆?*GPL-2.0-only 涓?AGPL-3.0 涓嶅吋瀹?*锛?   涓嶈鎶婂畠浠殑浠ｇ爜骞惰繘鏈伐绋嬶紱瀹炲湪瑕佸苟锛屾湰宸ョ▼寰楁暣浣撴敼鎴?GPL-2.0銆?2. 閾炬帴鍘傚晢鐨?NetworkDirect 搴擄紙`ndutil`/NDSPI锛夋病鏈夊奖鍝嶁€斺€斿畠浠笉鏄?copyleft 璁稿彲銆?

