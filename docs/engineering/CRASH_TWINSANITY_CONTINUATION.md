# متابعة Zmbor — 2026-11-04

**نقطة التحقق الأحدث:** رأس التنفيذ `f02d674aede120b81383e6ee1a51298b96edd6e9` على الفرع المحلي
`engineering/crash-twinsanity-full-system`. نسخة الأداء المرئية `sol_visible_fast1`، PID2884،
بإدخال عادي ودون مهلة تلقائية. نسخة التشخيص السابقة أُغلقت من المستخدم برمز0.
**VERIFIED:** تجاوز Vivendi وTTIDENT وظهور صورة Crash/Cortex؛ **NOT TESTED:** مشهد قابل للتحكم/Stage10.
**TESTED:** Unit ‏697/697 في نسخة الأداء. مجموعات CTest/E2E/IOP الكاملة الأخيرة تسبق Abort كما هو موضح أدناه.

## تصحيح مسار الأداء والاستفادة من RE4

بعد مراجعة سجل `Scale RE4 indirect-call discovery` رجعت للأصول المحلية بدل اعتماد ادعاء التسليم بغيابها.
**VERIFIED:** توجد78 binary تحت `D:/Project/Zmbor/thirdparty-data/ps2autotests/tests`: ‏55 EE و23 IOP.
شُغلت IOP مباشرة بالأداة الموجودة: **23/23 برنامجاً و1306/1306 سطراً مطابقاً، صفر regressions**؛
`sol_iop_hw.json`, `sol_iop_hw.md`, `sol_iop_hw.log`. تصنيف BLOCKED السابق بسبب غياب الملفات ملغى؛
إعادة EE55 ما زالت **NOT TESTED**. لا تُرقّى حالات components تلقائياً ولا تتغير قياساتها التاريخية.

**TESTED، مقارنة إعداد واحدة محدودة:** أُنشئ `build-ff-fast` مع نفس المصدر وإعدادات RelWithDebInfo،
FFmpeg ON، وgame /Od؛ الاختلاف `PS2X_ENABLE_AGRESSIVE_LOGS=OFF`.
شُغّلت النسختان تسلسلياً35 ثانية، دون sampler أو بناء ثقيل، بإدخال محايد وسلسلة إطارات.

| المقياس | تسجيل الدوال ON | OFF |
|---|---:|---:|
| elapsed runtime في summary |36.507 ثانية|36.466 ثانية|
| VSync ticks المبكرة |174|330|
| DMA/GIF |286/58|1066/214|
| CPU لكل العملية |39.9 ثانية|41.3 ثانية|
| ذروة RAM |270 MB|212 MB|
| الإغلاق |124 مهلة مقصودة|124 مهلة مقصودة|

تقدم VSync زاد نحو90% وانخفضت الذاكرة نحو21% في نافذة الإقلاع هذه. هذه ليست FPS لعب، ولا مقارنة CPU عند
نفس مقدار عمل الضيف؛ نسخة OFF أنجزت عملاً أكثر. فُحصت لوحتا `sol_perf_logging_{on,off}_sheet.png` بصرياً؛
نفس تسلسل الشعار/فحص البطاقة يظهر أبكر مع OFF. الإطارات التُقطت بالزمن، لذلك لا نزعم تطابقاً بايتياً عند tick متماثل.
السجلات `sol_perf_logging_{on,off}.{json,summary.txt,stdout.log,stderr.log}` و`*_frames.log`.
**TESTED:** Unit ‏697/697 للنسخة الجديدة، `sol_fast_unit.log`. أصبح التسجيل المكثف opt-in في CMake الافتراضي؛
السجلات العادية والأخطاء متاحة. لم تتغير دورات الضيف أو جدولة VBlank للحصول على النتيجة.

## تحقق Abort داخل اللعبة

**VERIFIED، `sol_visible_abort2`:** Abort استُدعي مرتين وأعاد1؛ الأول115.864 ثانية بعد Vivendi،
ثم عُثر على `TTIDENT.PSS` عند116.972 ثانية (حجم `0xf34004`)، والثاني175.204 ثانية.
ظهرت صورة Crash/Cortex في عرض GS. بقي تحذير طول غير صالح واحد، لذلك لا ندعي إغلاق كل مسارات EOF.
ظهرت أيضاً تحذيرات VU1 لتعليمة0xffffffff؛ سببها وأثرها على المشهد التالي لم يُحسما.
أغلق المستخدم النافذة طبيعياً؛ `sol_visible_abort2.ended.json` و`summary.txt` يسجلان exit0/user_close.
هذا يثبت تجاوز المقدمات بعد Abort، ولا يثبت لعباً تفاعلياً.

أوامر نسخة الأداء:

```powershell
& work/scripts/configure.ps1 -Dir build-ff-fast -Extra @(
 '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DPS2X_ENABLE_FFMPEG=ON',
 '-DPS2X_FFMPEG_ARCHIVE=D:/Project/Testing/work/deps/ffmpeg-n7.1-241205/ffmpeg-n7.1-241205-lgpl-amd64-shared.zip',
 '-DPS2X_GAME_GENERATED_DIR=D:/Project/Testing/work/crash_twinsanity/generated',
 '-DPS2X_GAME_OPTIMIZE=OFF', '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF')
cmake --build build-ff-fast --target ps2x_game ps2x_tests --parallel 8
& build-ff-fast/ps2xTest/ps2x_tests.exe
python -I tools/hwtest/iop_hwtest.py --tests D:/Project/Zmbor/thirdparty-data/ps2autotests/tests `
 --runner build/ps2xIOP/ps2_iop_autotest_runner.exe --out work/NEW_IOP_RESULT
```

الأولوية التالية: تحقق الإدخال في شاشة العنوان، ثم تتبع عائق VU/VIF للمشهد التالي إن تكرر؛
إعادة كل بوابات الانحدار بعد Abort وEE55 عند توقف التشغيل المرئي؛ ثم قياس GS بالدوال في عبء ثابت،
دون إعادة بناء اللعبة كاملة لكل تغيير تشخيصي صغير.

السجل المرحلي أدناه محفوظ تاريخياً؛ تُقدّم هذه النتائج الأحدث على الادعاءات السابقة.

رأس التنفيذ الأحدث: `051a034b65c51b04e6fab3218c81b1d7b64c605e` على الفرع
`engineering/crash-twinsanity-full-system`. رأس التوثيق اللاحق يُسجل في نتيجة Git النهائية.
التشغيل المرئي المتابع حالياً `sol_visible_abort2`، PID30132، نافذة `PS2-Recomp | SLUS_209.09`،
دون مهلة أو إدخال آلي. سجلاته وسلسلة إطاراته تثبت استمرار الإقلاع بعد إطلاقه، والمشغّل يبقى منتظراً حتى إغلاق المستخدم.
Enter=START، X=Cross، الأسهم=D-pad، WASD=العصا اليسرى عند تفعيل الوضع التناظري في اللعبة.

تحديث آخر: طُبّق واختُبر `sceMpegGetPictureAbort` بعد تحديد سبب تعثر نهاية الفيلم، وأُعيد توليد wrapper وبناء اللعبة.
**TESTED:** Unit ‏697/697 بعد إضافة abort (حالة أخرى داخل اختبار MPEG)، في `sol_abort_unit.log`.
**PARTIAL:** مجموعات CTest/E2E/IOP المدرجة أدناه تخص `71c0ffa` قبل abort؛ لم تُعَد بعد الإضافة الأخيرة.
**VERIFIED:** شُغلت النسخة الجديدة بنافذة مرئية وإدخال عادي بناءً على طلب المستخدم، PID27576 وعنوان
`PS2-Recomp | SLUS_209.09`؛ بيانات البدء `sol_visible_abort1.started.json` وسجلات `sol_visible_abort1.*`.
لا مهلة تلقائية، ولا script يضغط الأزرار في هذا التشغيل. أثر abort الحقيقي على الانتقال للفيلم التالي جارٍ التحقق.

آخر رأس تنفيذ مختبر: `71c0ffa0dbc1fe1389ea2c5d41647723fd5256ba`،
الفرع `engineering/crash-twinsanity-full-system`، دون نشر أو دمج.
**VERIFIED:** شعار/بطاقة ذاكرة وفيلم Vivendi مفكوك ومعروض؛ **NOT TESTED/لم تتحقق:** المرحلة10.
الاختبارات النهائية على RelWithDebInfo وFFmpeg ON: **697/697 Unit، 49 CTest ناجحاً + Z3 متخطّى،
6/6 E2E، 4/4 IOP، صفر فشل**. السجلات `sol_safe_unit.log`, `sol_safe_ctest.log`,
`sol_safe_e2e.log`, `sol_safe_iop.log`؛ بنيت أهداف الاختبارات وربطت بالـruntime الحالي قبل تشغيلها.
المقارنة OFF السابقة تخص نقطة692/692، ولم تُخلط مع قياس codec الخاص بـFFmpeg.

هذا السجل يستكمل التسليم عند `41d5a2edb0b631c3da1c34ff1b876542c2dd97a2` في
`D:\Project\Testing`، على الفرع `engineering/crash-twinsanity-full-system`.
كانت شجرة العمل نظيفة؛ لم يحدث reset أو clone أو push أو merge. البيانات واللقطات والسجلات الخاصة باقية في `work/`.

## الدفعة الأولى — القرص والإدخال

**VERIFIED:** أُعيد التحليل والترجمة، وأصبح `sub_002C3778_0x2c3778.cpp` يستدعي
`ps2_stubs::sceCdLayerSearchFile`. سجلا العملية: `sol_analyzer.log` و`sol_recomp.log` في مجلد diagnostics.

**VERIFIED، التشغيل `sol_cd1`، FFmpeg OFF:**

| القياس | النتيجة |
|---|---|
| البحث | استدعاء واحد عند 78,131.6 ms، `a0=0x70003ec0`، `a1=0xbca7e0`، `a2=0`، `a3=0xbca7f0`، `v0=1` |
| الملف | `\FMV\VIVENDI.PSS;1`؛ LSN افتراضي `0x100000`؛ حجم `0x6cc004`؛ `0xd99` قطاعاً |
| RPC القديم | لا طلب إلى `0x80000597`؛ استدعاء EE BindRpc الوحيد إلى `0x12345` |
| القراءة | `sceCdStInit(80,5,0x17c980)` ثم Start وSeek؛ قراءة 8 قطاعات = 16,384 بايت، بلا فشل |
| MPEG | DemuxPssRing استهلك 16,384 بايت؛ أربعة مقاطع ES؛ البداية `000001b3` |
| الفك | رسالة صريحة بأن FFmpeg معطّل؛ صفر إطار مفكوك؛ GetPicture ينتظر |
| الرسم | شعار/حقوق ثم فحص بطاقة الذاكرة؛ سواد لاحق؛ DMA=1621، GIF=325، VIF=6 |
| النهاية | 124، مهلة مقصودة؛ 152.5 ثانية للمضيف؛ CPU=168.2 ثانية؛ ذروة RAM=267 MB |

الملخص الخام `sol_cd1.summary.txt`، السجلان `sol_cd1.stdout.log` و`sol_cd1.stderr.log`،
القياسات `sol_cd1.json`، بصمات الإطارات `sol_cd1_frames.log`، واللوحة المحلية
`screenshots/sol_cd1_sheet.png`. فُحصت اللوحة بصرياً. أول لقطة شعار عند 3.1 ثانية، وفحص البطاقة عند 22.3 ثانية.
**المرحلة 9 باقية؛ لا دليل لعب تفاعلي.** عداد VSync النهائي 9000 يشمل الانتظار؛ لا يعني 60 إطار لعب في الثانية.

مراجعة ABI وبنية النتيجة اعتمدت على
[تعريفات PS2SDK](https://github.com/ps2dev/ps2sdk/blob/master/common/include/libcdvd-common.h)
و[تنفيذ EE](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/cdvd/src/libcdvd.c).
**PARTIAL:** البحث HLE على مجلد قرص مستخرج، بطبقة واحدة وLSN افتراضي؛ ليس تطبيق ISO9660 أو DVD مزدوج الطبقة كاملاً.
التاريخ في بنية النتيجة صفري؛ مطابقة أسماء الملفات تتضمن تسامحاً موروثاً؛ أخطاء العتاد الدقيقة غير مثبتة.

الإصلاحات الجديدة:

- `1ac7b7e`: فحص كامل نطاق نتيجة البحث 32 بايت قبل الكتابة، لجميع بدائل RAM وscratchpad؛ تشخيص آمن للمؤشرات غير المعينة.
  اختباران جديدان. فحص طفرة يعطّل الحاجز في نسخة معزولة من جسم المساعد الفعلي كشف ست حالات تجاوز؛
  `sol_cd_bounds_candidate.log` نجاح، و`sol_cd_bounds_mutant.log` فشل متوقع. لم تُعدَّل المخرجات الذهبية.
- الالتزام نفسه: بناء فهرس الأسماء رقمياً باستخدام `lexically_relative` بدلاً من `filesystem::relative`.
  **VERIFIED:** الحل القديم يفشل بـ Access denied أثناء canonicalization رغم إمكانية قراءة الملف؛ أدى ذلك إلى فشل اختبار
  `MV_16.PSS` مقابل `MV_016.PSS`. يحتفظ `sol_numeric_probe.log` بالمقارنة، والاختبار الأصلي ينجح بعد الإصلاح.
- `11430fb`: إدخال حتمي عبر `ZMBOR_PAD_SCRIPT`، بجدول VBlank أو milliseconds، ضغط وإطلاق، محاور 0–255، منفذين،
  تسجيل الانتقالات المقروءة، وإعادة تشغيل. أربعة اختبارات جديدة؛ طفرة إلغاء عكس active-low تفشل كما ينبغي.
  [صيغة التشغيل](../../tools/game_pipeline/PAD_PLAYBACK.md).
  **TESTED:** parser ودمج HLE؛ **NOT TESTED:** استجابة Crash التفاعلية؛ **PARTIAL:** لا يغطي مسار SIO2 منخفض المستوى.

## الانحدار

| المجموعة | قبل الإصلاح | بعد الإصلاح |
|---|---|---|
| Unit | 685 نجاحاً / 686؛ فشل مطابقة الأصفار | 692/692، صفر فشل |
| CTest غير E2E/hwtest | 48 نجاحاً، فشل hwprobes، تخطّي Z3 | 49 نجاحاً، 1 تخطّي Z3، صفر فشل |
| E2E تسلسلي | 6/6 | 6/6 |
| IOP | 4/4 | 4/4 |

أُعيدت Unit مع TEMP محلي قبل الإصلاح وظل فشل الاسم، أي أنه لم يكن مشكلة ملفات مؤقتة.
فشل hwprobes كان إذن وصول إلى مجلد TEMP الخاص ببيئة التنفيذ، وحُل باختيار `work/sol_tmp` دون تغيير الاختبار.
السجلات `sol_{unit,ctest,e2e,iop}_baseline.*` و`sol_unit_localtmp.log` و`sol_hwprobe_localtmp.log`
و`sol_{unit,ctest,e2e,iop}_candidate.log` تحفظ التجارب منفصلة.
**BLOCKED:** إعادة اختبارات ps2autotests لأن ELFs غير موجودة. **NOT TESTED:** CI بعيد وLinux وsanitizers.

## FFmpeg والتنميط — نقطة العمل التالية

**VERIFIED:** SHA-256 للأرشيف المحلي يطابق CMake:
`B985887DBCA8C0A9A4C47F9D2BAC7EC3AB994335B1B7FE7000BDB1DCD772AC68`.
البرنامج المرفق يعرّف x64 MSVC، وlibavcodec 61.19.100، ولا تعرض خياراته enable-gpl أو enable-nonfree.
البناء `build-ff` أُعيدت تهيئته إلى RelWithDebInfo وgame `/Od` لتوفير PDB وتسريع التكرار؛
سجلا الإعداد والبناء `sol_ff_configure.log` و`sol_ff_build.log`.
هذه ليست مقارنة أداء مضبوطة مع Release السابق؛ تختلف خيارات runtime.

**TESTED:** أداة خارجية في `tools/profiling` تسجل مكدسات جميع الخيوط وتكاليف CPU الإجمالية مستقلة عن العينات،
مع عدد عينات شامل وحصري لكل دالة وخيط، وحفظ المكدسات الخام. اختبار الدالة الساخنة/الأب والخيوط المنتظرة ناجح،
واختبار تصادم المسارات يحافظ على بصمات جميع المخرجات. الأدلة `sol_profiler_selftest_v3.*`.
العينات زمنية تشمل الانتظار وليست نسب كلفة CPU. محاولة WPR العادية فشلت بـ `0xc5585011`؛ لم تُغيَّر سياسة النظام.
لا يوجد حتى هذه النقطة ترتيب مقاس لاختناقات اللعبة أو تحسين أداء جديد.

**PARTIAL، تحقيق جارٍ:** قارئ ترويسة MPEG الأول ثم إعادة Create بالمخزن الكامل سلوك ضيف مقصود؛
لا يصح الاحتفاظ بحالة الفك دائماً عبر Create. callbacks غير stream المسجلة بـ AddCallback ليس لها مسار تنفيذ ظاهر
في GetPicture الحالي. يجب إثبات أثر ذلك في تشغيل FFmpeg قبل تعديل المضخة.

## تشغيل FFmpeg الأول والتنميط الفعلي

**TESTED:** اكتمل `build-ff` فعلياً مع DLLs وPDB، ونجحت Unit ‏692/692.
**VERIFIED:** التشغيل `sol_ff1` وصل إلى ملف الفيديو نفسه وإلى FFmpeg parser؛ 4 قطع ES أنتجت packet واحدة
وصفر إطار. أعادت اللعبة Create بعد قراءة الترويسة ثم بقي GetPicture عند `sawInput=0`.
لا يكفي تمكين المكتبة لتشغيل الفيلم. خرج التشغيل بالمهلة 124 بعد 155.3 ثانية للمضيف، CPU=163.6 ثانية،
وذروة RAM=275 MB؛ الشاشة الأخيرة سوداء. هذا تشغيل مع تنميط، وليس benchmark قابلاً للمقارنة مع التشغيل السابق.

**VERIFIED، نافذة عينات 60.0199 ثانية بعد تأخير 5 ثوانٍ:**
71,733 لقطة مكدس لجميع الخيوط، صفر فشل سياق أو فتح خيط؛ CPU للعملية=59.4844 ثانية.
الخيط الأعلى CPU (`20308`) استهلك 51.2812 ثانية وله 2,918 عينة، مقابل 5.85938 و1.625 ثانية للخيطين التاليين.
الخيوط المنتظرة فُصلت عن خيط EE عند تحليل الجدول، ولم تُجمع نسبها لتوهم اختناقاً.

| الدالة على خيط EE | عينات حصرية | عينات شاملة |
|---|---:|---:|
| `ZwWriteFile` | 1169 | 1169 |
| `GSCpuBackend::WritePixel` | 254 | 508 |
| `GSCpuBackend::DrawTriangle` | 73 | 569 |
| `GSCpuBackend::DrawSprite` | 85 | 554 |
| `GSCpuBackend::SampleTexture` | 51 | 325 |
| `IopEmulator::Impl::runCycles` | 0 | 92 |
| `PS2Runtime::executeVU0Microprogram` | 1 | 31 |

هذه عينات wall time وليست نسب CPU. تتداخل الشموليات ولا تُجمع. تكشف المكدسات `ps2_log::log_entry/exit`
وflush في طريق الكتابة؛ `PS2X_ENABLE_AGRESSIVE_LOGS=ON` يفعّل تتبع دخول وخروج الدوال، وكان الملف 262,823,841 بايت.
حُفظ الملف في `sol_ff1.function-log.txt`. هذا اختناق تشخيص مقاس في هذا الإعداد، لا إثبات لسرعة اللعب أو لترتيب
كلفة الأنظمة أثناء مشهد تفاعلي. يتطلب تقييم تعطيل التسجيل تشغيلين بلا sampler وبخيارات أخرى متطابقة.
المصادر الخام `sol_ff1.profile.{functions.csv,stacks.csv,threads.csv,meta.txt}`.

الإصلاح المرشح التالي يعالج nodata callback وتشغيل stream callbacks المتداخلة قبل عودة Demux.
**TESTED:** Unit مع المرشح ‏693/693. تعطيل nodata وحده أسقط الاختبار الجديد (692/693)؛
اختبار fixture الاصطناعي يتحقق من النسخ والترتيب والرجوع والعرض في ذاكرة الضيف، ولا يزعم أنه اختبار codec حقيقي.
احتفظت السجلات أيضاً بمحاولة استعادة أعادت timestamp أقدم فتجاوز Ninja إعادة ترجمة الملف؛
هذه نتيجة mutant باقية في `sol_mpeg_mutation_restored_unit.log` وليست نجاح استعادة.
أُعيد البناء بطابع زمني جديد في ملفات `*_v2.log` قبل أي تشغيل لعبة للمرشح.

## أوامر التشغيل

## متابعة callbacks وDMA

**VERIFIED، `sol_mpeg1`:** إصلاح nodata الأول جعل اللعبة تبدأ DMA4 نحو IPU عند 89.34 ثانية:
`CHCR=0x101`, `MADR=0x44f540`, `QWC=0x200` (8192 بايت). عاد كل stream callback، لكن nodata تكرر
8559 مرة لأنه يفحص STR ويبقى النقل جارياً. DMA=1622 بدلاً من1621، دون إطارات جديدة؛ الخروج124 عند155.8 ثانية،
CPU169.8 ثانية. هذا يحدد تعارض حدود HLE/العتاد: FIFO ثمانية qwords يمتلئ، ولا تصدر أوامر فك IPU لأن GetPicture
استُبدل بـHLE. لم يُمسح CHCR لتجاوز المشكلة.

**TESTED، الالتزام `337b878`:** واجهة `drainIpuInputForHle` تستهلك FIFO الحقيقي ثم تسمح للمحرك المعتاد
بتغذيته من النقل المعلق وإنهائه طبيعياً. اختبارات512qword تثبت ترتيب كل بايت وحدود القراءة وMADR/QWC/STR/CIS4،
ولا تختلق interrupt لفك IPU. الأوامر الجارية والبتات المستهلكة جزئياً والخرج غير المقروء تبقى ملك مسار العتاد.
ثلاث حالات اختبار جديدة، Unit ‏696/696 في `sol_ipu_candidate_unit.log`.

صُححت أيضاً مراجعة مسار nodata: callbacks المتتابعة تعمل واحداً تلو الآخر وتعيد استخدام عمق مكدس واحد؛
اختبار100callback يمنع حجز16KiB لكل عنصر من دفعة متسلسلة. ويحافظ retry على نتيجة إلغاء الانتظار السالبة.

**VERIFIED من تعليمات مكتبة الضيف، لا من قياس عتاد:** AddCallback يعيد عنوان الدالة السابقة ويستبدل التسجيل؛
AddStrCallback يستبدل مفتاح stream نفسه ويعيد الدالة السابقة؛ Create الناجح ينشئ جدول callbacks جديداً.
هذه الدلالات موثقة بتفكيك محلي `sol_mpeg_lifecycle_disasm.log`، وصُحح اختبار stub السابق الذي كان يتوقع handles
متزايدة. لا أُضيفت عناوين أو offsets خاصة باللعبة إلى تنفيذ جدول callbacks العام. حُفظ GP وقت التسجيل.

مرشح الربط يختار استهلاك DMA في GetPicture عند استعمال nodata، ويمنع فك payload في Demux مرة ثانية في ذلك الوضع.
يعيد تغذية FFmpeg من البايتات التي استهلكتها FIFO فقط. EOF المنتج منفصل عن EOF decoder؛
لا يفترض نفاد مخزن ES بمجرد انتهاء قراءة القرص. **PARTIAL:** callback يعود1 دائماً عند نهاية الفيلم قد يحتاج
تحديد EOF إضافياً؛ timestamps من PES لا تنتقل تلقائياً عبر بايتات DMA؛ fallback يعتمد معدل sequence header.
اختبار DMA الجدولي يستخدم fixture إطار محقوناً لاختبار التسليم، لا يُزعم أنه تحقق codec؛
نتيجة codec الحقيقية تُحسم من تشغيل اللعبة اللاحق.

## أوامر البناء الأخيرة

### السبب الجذري الأخير وإصلاحه

**VERIFIED:** `sol_safe_input1` استمر245.6 ثانية وخرج124 دون Access violation، بذروة276MB بدلاً من586MB
في التشغيل المنهار. قرأ7,129,088 بايت من الفيلم عبر436 قراءة، وأظهر Vivendi. لكنه بقي عند نهاية الفيلم؛
لم تُقرأ أحداث الأزرار بعد الحالة المحايدة، فلا يُزعم استجابة START/CROSS أو المحاور داخل اللعبة.
سجل الأطوال غير الصالحة أظهر `data=0x44f501`, `count=0xffffffff`.

تتبع الضيف أثبت أن `-1` هو sentinel من قارئ الملف عند بلوغ حجمه، ثم تستدعي nodata
`sceMpegGetPictureAbort`. كان GetPicture HLE يتجاهل الإلغاء ويستدعي المنتج مجدداً بعد النهاية.
بصمة الدالة0x2b7ca0، بحجم16 بايت، هي `929d5c4ab5abc2bff17c24561aafa4e9e7070ad1`،
مطابقة تماماً لسجل `sceMpegGetPictureAbort` الخاص بـSDK3.x في قاعدة التواقيع الموجودة.
أدلة التفكيك `sol_mpeg_abort_inner_disasm.log` و`sol_mpeg_abort_reset_disasm.log` و`sol_refill_eof_investigation.txt`.
المكتبة تعيد-1 منGetPicture عند الإلغاء، وتبقي العلم حتىReset/Create؛ Abort يعيد1. ليس هذا دليلاً على EOF عام للقرص.

أُضيف معالج API عام بعلم لكل instance، وفحص قبل استهلاك DMA/عرض frame/إعادة استدعاء producer، وإيقاظ انتظار MPEG
بنتيجة سالبة. لا يُفرغ DMA اصطناعياً ولا يفترض انتهاء القرص. الاختبار يثبت استدعاء المنتج مرة واحدة،
عودة GetPicture السالبة رغم نجاح callback، استمرار الإلغاء في retry، ثم التعافي بعدReset.

**VERIFIED:** المحلل محافظ ولا يقبل توقيعاً بأقل من256 static bits؛ بقي هذا الشرط دون إضعاف.
`tools/game_pipeline/bind_sdk_stub.py` يضيف annotation إلى TOML بعد فحص SHA-1 للـELF،
ثم أعاد recompiler توليد wrapper باستدعاء المعالج. تغير فقط `sub_002B7CA0_0x2b7ca0.cpp` و`ps2_recompiled_stubs.h`؛
حُفظت النسخ السابقة في `archive/sol_abort_bound_regen`. السجلات `sol_abort_bound_recomp.log` و`sol_abort_game_build.log`.
**TESTED:** البصمة الخاطئة تُرفض دون تغيير config (`sol_bind_negative_v2.log`)، والصحيحة تُربط
(`sol_bind_positive_v2.log`). Python الفعلي3.10.11؛ أُضيف توافق دون اعتماد tomllib الإلزامي.
هذا ربط تحليل خاص بعنوان الدالة في هذه النسخة بعد التحقق من هوية المكتبة، وليس عنوان لعبة مزروعاً في الـruntime.

### القيود والخطوات التالية

1. **PARTIAL:** إثبات استدعاء Abort في التشغيل المرئي، اختفاء أطوال -1 بعد EOF، ثم تحديد الفيلم التالي أو القائمة من إطارات فعلية.
2. **NOT TESTED بعد Abort:** إعادة بناء وربط أهداف E2E وأهداف CTest وIOP ثم تشغيلها دون بناء ثقيل متزامن.
   الاختبارات الكاملة الأخيرة عند 71c0ffa ناجحة، والـUnit عند 051a034 ناجحة 697/697.
3. **NOT TESTED:** تفاعل القائمة، مشهد قابل للعب، حركة/كاميرا/قفز/تصادم وتقدم. لا تُرقّى Stage10 بسبب فيديو متحرك.
4. **PARTIAL:** آلية الإدخال الحتمية جاهزة ومختبرة، لكن تشغيل script الأخير لم يقرأ سوى neutral قبل تعثر EOF؛
   لم تثبت الأزرار داخل مشهد تفاعلي. التشغيل المرئي الجديد يستخدم إدخال المستخدم العادي.
5. **PARTIAL:** التنميط وفر مكدسات شاملة/حصرية زمنية وCPU لكل خيط؛ WPR CPU sampling لم يعمل في البيئة الحالية.
   لا يوجد تحسين أداء جديد مُقاس بمقارنة مضبوطة، ولا قيم CPU inclusive/exclusive مُختلقة.
   أول تجربة لاحقة: نفس المصدر والإعداد مع/دون AGRESSIVE_LOGS، دون sampler، وقارن زمن أول شاشة وبصمات الإطارات؛
   بعدها نمّط GS/IOP/EE في مشهد تفاعلي. لا يُفسَّر VBlank أثناء الانتظار بأنه سرعة لعب.
6. **PARTIAL:** FFmpeg يفك MPEG2 ويحوّل RGBA ويُعرض الفيلم الأول؛ ربط PTS عبر DMA، نهايات streams الأخرى،
   RAW8، دقة IPU العتادية، ودعم تبديل ملكية الإدخال المتأخر تحتاج عملاً. الصوت لم يُسمع/يُتحقق منه.
7. **BLOCKED:** ps2autotests محلياً لغياب ELFs. **NOT TESTED:** CI للفرع المحلي، sanitizers وLinux في هذه المتابعة.
8. **VERIFIED:** لم تُغير golden expectations، ولم تُرفع بيانات اللعبة أو السجلات أو اللقطات، ولم يحدث push/merge.

إعادة تشغيل مرئي يدوياً بعد إغلاق العملية الحالية:

```powershell
Remove-Item Env:PS2X_HIDDEN_WINDOW, Env:ZMBOR_RUN_SECONDS, Env:ZMBOR_PAD_SCRIPT -ErrorAction SilentlyContinue
Set-Location 'D:\Project\Testing\work\crash_twinsanity\disc'
& 'D:\Project\Testing\build-ff\ps2xRuntime\ps2x_game.exe' '.\SLUS_209.09'
```

للتوليد القابل للإعادة استخدم أداة pipeline المحدثة؛ فهي تتحقق من توقيع Abort قبل ربطه. لا يُستخدم ملف TOML الآلي
غير المعلّق وحده لهذا SDK قصير الدالة. إذا رفضت البصمة فلا تتجاوز الرفض؛ راجع هوية ELF/نسخة SDK أولاً.

**VERIFIED، `sol_dma2`:** ظهرت إطارات فيلم Vivendi Universal Games في عرض GS، مع decode بحجم640×480.
سلسلة اللقطات المحلية `sol_dma2_progress_sheet.png` فُحصت بصرياً: الشعار يتحرك ثم يستقر، بعد شاشة البطاقة.
اكتملت سلسلة disc→PSS→guest ES callbacks→DMA4/FIFO→FFmpeg→RGBA guest→GS→display للفيلم الأول.
المرحلة9 باقية؛ لا قائمة أو شخصية قابلة للتحكم.
انتهى التشغيل لاحقاً بـAccess violation ‏`-1073741819` بعد123.7 ثانية، CPU133.5 ثانية، ذروة586MB؛
لا ملخص runtime لأن المضيف انهار. هذه نتيجة **FAILED** لاحقة لنجاح الفيديو، ولا تُخفى خلف timeout.

**VERIFIED، إعادة الإنتاج بالمصحح:** `sol_dma_crash_debug.cdb.log` يمسك `c0000005` في
`memmove←vector::insert←appendPssBytes←appendGuestBytes←sceMpegDemuxPssRing`.
وصل النسخ إلى scratchpad مع مدى RAM، فقرأ خارج التخصيص؛ فحص المدى الكامل والأطوال هو الإصلاح الجاري.
الخلل ليس فشلاً في decoder FFmpeg. rawdebugconsole محفوظ منفصلاً؛ يتضمن C++EH اعتيادية خاصة بنقل تحكم scheduler.

حدود التحقق الحالية محفوظة أيضاً: محاولة `sol_dma_bridge_unit.log` الأولى انهارت برمز
`-1073741819` بسبب خطأ تهيئة في اختبار DMA الجديد (استعمل memory غير مهيأة). لم يبدأ تشغيل اللعبة بعدها.
صُحح الاختبار ليهيئ ذاكرة runtime ويستخدم RDRAM نفسها للجدولة والـDMA والصورة؛ نتائج إعادة البناء
في `sol_dma_bridge_build_v2.log` و`sol_dma_bridge_unit_v2.log`.
أُضيف stream MPEG2 صغير مولّد من لون أحمر16×16، دون أصول لعبة، لاختبار codec حقيقي عبرDMA في builds FFmpeg.
المسار غير المتاح في FFmpeg-OFF لا يُحسب تحقق codec. التسجيل المتأخر لـnodata بعد تغذية مباشرة سابقة
يرجع خطأ HLE صريحاً عند GetPicture لأن الانتقال قد يعيد فك البايتات؛ دعم هذا الانتقال العام **PARTIAL**.

```powershell
& work/scripts/configure.ps1 -Dir build-ff -Extra @(
  '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DPS2X_ENABLE_FFMPEG=ON',
  '-DPS2X_FFMPEG_ARCHIVE=D:/Project/Testing/work/deps/ffmpeg-n7.1-241205/ffmpeg-n7.1-241205-lgpl-amd64-shared.zip',
  '-DPS2X_GAME_GENERATED_DIR=D:/Project/Testing/work/crash_twinsanity/generated',
  '-DPS2X_GAME_OPTIMIZE=OFF')
cmake --build build-ff --target ps2x_tests ps2x_game --parallel 8
& build-ff/ps2xTest/ps2x_tests.exe
& tools/game_pipeline/crash_pipeline.ps1 -SkipRecompile -SkipBuild -BuildDir build-ff `
  -RunSeconds 180 -Tag NEW_DMA_TAG -Trace all -SeriesMs 2000
```

```powershell
. work/scripts/env.ps1
$env:TEMP = 'D:\Project\Testing\work\sol_tmp'
$env:TMP = $env:TEMP
$env:PS2X_HIDDEN_WINDOW = '1'
cmake --build build --parallel 8 --target ps2x_tests ps2_analyzer ps2_recomp ps2x_game
& build/ps2xTest/ps2x_tests.exe
ctest --test-dir build/ps2xTest -LE 'hwtest|e2e' -j 6 --output-on-failure
ctest --test-dir build/ps2xTest -L e2e -j 1 --output-on-failure
ctest --test-dir build/ps2xIOP -j 4 --output-on-failure
& tools/game_pipeline/crash_pipeline.ps1 -RunSeconds 150 -Tag NEW_UNIQUE_TAG -Trace all -SeriesMs 3000
```

الأداة ترفض الآن إعادة استخدام tag أو مسح الأدلة السابقة، وتفحص خروج analyzer قبل متابعة المراحل.
بعد توليد الكود يمكن استعمال `-SkipRecompile`، وبعد البناء `-SkipBuild`.
