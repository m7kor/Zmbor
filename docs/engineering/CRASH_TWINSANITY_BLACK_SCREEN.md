# Crash Twinsanity — تحقيق الشاشة السوداء (الجلسة 2)

التاريخ: 2026-11-04. الفرع `engineering/crash-twinsanity-full-system`. كل رقم هنا من تشغيل فعلي في هذه الجلسة
(MSVC 14.44، Release، الكود المولَّد `/Od` ما لم يُذكر خلاف ذلك). لا يحتوي الملف على بيانات من اللعبة سوى العناوين
والأعداد وأسماء الملفات.

## 1. النتيجة

**السبب الجذري المؤكَّد:** خدمة RPC من الـ IOP إلى الـ EE غير مطبَّقة في مُحاكي الـ IOP.
`STREAM.IRX` (وحدة البث الخاصة باللعبة على الـ IOP) يربط نفسه بخادم RPC على الـ EE (`sid=0x12344321`، يسجّله خيط EE
رقم 2 عبر `sceSifRegisterRpc` + `sceSifRpcLoop`). المُحاكي كان يعامل `sifcmd` ordinals 15 و16 كأنهما 14
(`sceSifInitRpc`) فيرجع 0 دون ربط، فيبقى `cd.server == 0` وتدور حلقة الإعادة في الـ SDK:
**153,542** استدعاء `sceSifBindRpc` ثم يستسلم الـ IOP، بينما يستمر الـ EE في الاستعلام عن حالة `STREAM`
(**62,778** استدعاء `sceSifCallRpc` في 60 ث) دون تقدّم، ولا تُرسم إلا شاشة سوداء.

جدول تصدير `sifcmd` في ps2sdk: 14 `sceSifInitRpc`، 15 `sceSifBindRpc`، 16 `sceSifCallRpc`، 17 `sceSifRegisterRpc`،
18 `sceSifCheckStatRpc`؛ المُحاكي كان أصلاً يستخدم 17/19/22/24 وفق هذا الجدول.

**بعد الإصلاح** (الالتزام `116554f`): ربط واحد بدل 153,542؛ 88 استدعاء IOP→EE مكتمل؛ قراءات القرص 34→239
(1,803 قطاعاً)؛ اللعبة تعرض شاشة الشعار والحقوق ثم شاشة «Checking memory card». **المرحلة 9 تحققت.**

## 2. الفرضيات والتجارب

| # | الفرضية | القياس المميِّز | النتيجة |
|---|---|---|---|
| H1 | اللعبة لا تقرأ أصولها (`CRASH.BD`) | تدقيق I/O على الـ EE (`ZMBOR_TRACE_IO`) | **مرفوضة جزئياً**: الـ EE لا يفتح سوى `rom0:ROMVER`، لكن عدّاد cdvdman على الـ IOP أظهر أن `STREAM.IRX` يقرأ `CRASH.BH` و`CRASH.BD` عبر `sceCdSearchFile`/`sceCdRead` على الـ IOP (34 قراءة، 247 قطاعاً، 0 فشل). استنتاج الجلسة 1 «اللعبة لا تقرأ CRASH.BD» كان خاطئاً لأن التدقيق لم يكن يرى جانب الـ IOP. |
| H2 | خلل في نقل RPC من الـ EE إلى الـ IOP | عدّاد `rpc_call sid/fn` في جسر الـ IOP | **مرفوضة**: `sid=0x12345 fn=0` خُدم 36,220 مرة، 0 بلا خادم |
| H3 | الـ IOP لا يستطيع الاتصال بالـ EE | عدّاد استدعاءات `sifcmd` حسب الترتيب، ثم حسب الـ sid | **مؤكَّدة**: `sifcmd:15` = 153,542 كلها إلى `sid=0x12344321` (خادم EE مسجَّل) |
| H4 | الإطار الأسود من مسار العرض على المضيف | `PS2X_DUMP_FRAME` + سلسلة إطارات | **مرفوضة بعد الإصلاح**: إطارات غير سوداء ظاهرة مباشرة من ذاكرة الـ GS |
| H5 | قيمة `sceMcGetInfo` الأولى (0 بدل -1 حسب libmc.h) | تجربة A/B | لا أثر ملحوظ (الجلسة 1، قبل إصلاح H3)؛ لم تُعدَّل |
| H6 | تشغيل FMV مع فك MPEG معطَّل | تتبع HLE | الشاشة السوداء الأولى لم تكن FMV؛ لكن بعد شاشة البطاقة (~77 ث) تستدعي اللعبة `sceIpuInit`/`sceMpegInit` (انظر §6) |

## 3. ما دخل خط الرسوميات وما خرج منه (بعد الإصلاح، تشغيل `gs1`، 60 ث، `ZMBOR_TRACE_GS`)

| الهدف (FRAME) | المصدر (TEX0) | النوع | العدد | المعنى |
|---|---|---|---|---|
| FBP 56، PSMCT32 | — | TRIANGLE مع ABE | 194,050 | رسم المشهد في المخزن 32-بت |
| FBP 56 | TBP 14336..16128، PSMT8 (CLUT) | TRIANGLE/SPRITE | 136 + 17,010 + … | نصوص/صور مرفوعة عبر نقل GS (64×160، 9 مرات) |
| FBP 0، PSMCT16، عرض 512 | TBP 1792، PSMCT24 | SPRITE شرائط | 3,664 | نسخ المخزن 32/24-بت إلى مخزن العرض 16-بت |
| FBP 320/376 | TBP 5376/7168، PSMZ24 | SPRITE | 7,328 لكل | تمريرات مساعدة بقناع FBMSK |

سجلات العرض عند النهاية: `DISPFB1/2 = 0x11000` (FBP 0، FBW 8، PSMCT16)، `PMODE = 0x8023`.
DMA: GIF chain 229، VIF1 chain 229، VIF0 chain 688.

## 4. تطوّر الشاشة (تشغيل `series1`، 120 ث، إطار كل 3 ث)

| الزمن (مضيف) | المحتوى | بصمة الإطار |
|---|---|---|
| 0 ث | أسود (قبل الرسم) | `9c70d3e95c` |
| 3–16 ث | شعار Crash Twinsanity + نص الحقوق | `2ce9…`, `7d7f…`, … |
| 19 ث | تلاشٍ متقاطع | `04cbe3d9cf` |
| 22–53 ث | «Checking memory card (PS2) in memory card slot 1 …» | `e4d114fd82` (ثابت) |
| 56 ث | انكماش الشاشة | `30125566bf` |
| ≥59 ث | أسود | `9c70d3e95c` |

لقطات (محلية، غير مُلتزمة): `work/crash_twinsanity/screenshots/series1_sheet.png`، `gs1.frame.png`.

## 5. تدقيق خدمات HLE على الـ EE (تشغيل `post1`، 100 ث)

الأكثر استدعاءً: `sceSifCallRpc`/`sceSifCheckStatRpc` 1,215 (إلى `STREAM sid 0x12345`)، `_sceIDC` 982،
`scePadGetState` 384، `sceGsSyncV` 384، `scePadRead` 374، `sceMcSync` 325، `sceMcGetInfo` 320،
`sceSifBindRpc` 147. استدعاءات تهيئة لمرة واحدة: `sceCdInit`، `sceSifRebootIop`، `_sceSifLoadModule`×8، `sceMcInit`،
`scePadInit`، `sceSdRemoteInit`، `sceGsResetGraph`، `sceMcGetDir` (~53 ث)، `sceGsSyncVCallback`/`sceIpuInit`/`sceMpegInit` (~77.7 ث).
Syscalls: `0x64` (976 مرة) هو الأكثر؛ البقية تهيئة.

## 6. ما تبقّى بعد شاشة البطاقة

- ~53 ث: `sceMcGetDir` (بحث عن حفظ).
- 56–77 ث: لا نشاط GIF/DMA جديد ولا قراءات قرص جديدة؛ الشاشة سوداء.
- 77.7 ث: `sceIpuInit` + `sceMpegInit` — اللعبة تبدأ تهيئة MPEG/IPU (مقاطع الافتتاح). مع `PS2X_ENABLE_FFMPEG=OFF`
  ينتج فك MPEG إطارات بديلة، لذا المرحلة المرئية التالية متوقفة على تفعيل فك الفيديو (يتطلب تنزيل FFmpeg: **BLOCKED**
  بانتظار موافقة المستخدم).
- لم يُلاحظ أي طلب لملف `.PSS` حتى 100 ث.

## 7. الأداء: `/Od` مقابل `/O2` (نفس المصدر `4dc1dc1`، 90 ث، دون تتبع، إطار كل 1 ث)

| المقياس | `/Od` (`build`) | `/O2` (`build-o2`) |
|---|---|---|
| زمن بناء `ps2x_game` | 161 ث (`-j8`، الجلسة 1) | 1,137 ث (`-j8`، دون حمل آخر) |
| حجم الملف التنفيذي | 224,261,120 B | 56,631,296 B |
| VSync للضيف | 1,109 في 92.0 ث (12.1/ث) | 1,430 في 93.4 ث (15.3/ث) |
| أول إطار غير أسود | 1.1 ث | 1.1 ث |
| أول إطار بطاقة الذاكرة الثابت | 22.4 ث | 19.6 ث |
| CPU / ذروة الذاكرة | 113.3 ث / 209 MB | 119.0 ث / 181 MB |
| مخرجات الضيف | DMA 1,621، GIF 325، VIF 6 | متطابقة؛ بصمات الإطارات متطابقة |

الإبطاء: VBlank يُطلق عند بلوغ دورات الضيف (~4.9 مليون دورة EE لكل إطار) **و**موعد 16.667 ms معاً
(`EeScheduler::processDueDeadlines`)، فمعدل 12–15/ث يعني أن إنتاج دورات الضيف هو المحدِّد. لم يُجرَ تنميط على مستوى
الدوال؛ لا ادعاء بموضع عنق الزجاجة.

## 8. أدوات التشخيص المضافة

`ZMBOR_TRACE_HLE/SYSCALL/IO/GS/ALL`، `ZMBOR_SUMMARY_FILE`، `ZMBOR_RUN_SECONDS`، `ZMBOR_FRAME_SERIES(_MS)`،
وعدّادات IOP في الملخص (`import lib:ord:disposition`، `rpc_server`، `rpc_call`، `iop_client`، `cd_search`، `cd_read`).
الأداة: `work/scripts/crash_pipeline.ps1 -Tag <t> -Trace all -SeriesMs 3000` و`work/scripts/contact_sheet.py`.
