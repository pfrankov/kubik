# Иллюстрации для книжки

Созданы встроенным Imagegen отдельными сценами. `hello-tess.png` задаёт общий
вид Кубика: изометрический корпус, экран спереди, верхняя и правая грани.
Это условные схемы, не фотографии. По уточнению пользователя сверху находятся
три **круглые** кнопки: BOOT слева, PWR по центру, KEY справа при взгляде на экран.
Расстояния и точные положения на корпусе ещё требуют физического подтверждения.

Стиль: толстый чёрный внешний контур, более тонкие швы, белая верхняя грань,
тёмно-мятная правая грань и плоское мятное пятно под предметами. Палитра — чёрный,
тёмно-мятный `#17695F` и белый; без текстур и градиентов. Tess — куб внутри куба
из точек, без лица и конечностей. Все сцены используют один референс корпуса.

Подписи кнопок набраны PT Sans отдельно: SVG-выноски на странице 2 `booklet.html`
и в начале `START.html`, до первого действия с кнопками. Поэтому подписи остаются
чёткими при печати; каждая линия приходит к соответствующей кнопке.

## hello-tess.png

Brief: bold editorial product illustration, isometric Kubik with black screen,
dotted mint Tess, white top and solid dark-mint right side. Strong outer contour,
fine seams, flat mint ground shape. Exactly three raised circular top buttons,
left to right BOOT, PWR, KEY. Keep the established body and composition; no labels.

Оригинал: `exec-ede86575-6e99-4498-b8d9-82c3c70e1c4c.png`.

## prepare-agent.png

Brief: laptop and two-antenna home router, black and dark-mint editorial drawing
on white. Bold silhouettes and lighter internal detail, matching the Kubik art.
Mint conversation lines on the laptop screen; compact horizontal composition.

Оригинал: `exec-5bf61e2e-717d-4a06-8a75-311fd8f42044.png`.

## scan-setup.png и scan-setup-next.png

Brief: preserve the isometric Kubik reference, its three circular top buttons
and bold three-colour style. Show the actual setup-screen arrangement from the
native renderer reference: large central QR matrix on white, heading above it,
short network name or address below. No phone or empty scan brackets.

Первый экран: `SCAN TO JOIN WI-FI`, пример сети `KUBIK-4F2A`.
Оригинал: `exec-6b526ad9-8758-4930-8ebc-f77471b0fbc8.png`.

Второй экран: `SCAN AGAIN FOR SETUP`, адрес `192.168.4.1`.
Оригинал: `exec-e6d25b1d-b9f6-43ea-af1f-f11c67e2bbc6.png`.

Референсы получены из `firmware/sim/sim_text.c` через настоящий рендерер прошивки.
Матрицы на иллюстрациях — примеры внешнего вида, не коды конкретного покупателя.
Подпись в книжке направляет сканировать экран устройства, а не бумагу.

## first-conversation.png

Brief: friendly person speaking towards the same isometric Kubik. Black hair,
dark-mint shirt and voice arcs, dotted Tess on the screen, three circular top
buttons. Strong black outer contour, restrained detail, white background.

Оригинал: `exec-5f683754-7cef-4b71-bd79-1ca65862dec4.png`.

Выбранные оригиналы скопированы в `assets/`; проект не зависит от каталога Codex.
Контрольные суммы ресурсов находятся в `print-manifest.json`.
