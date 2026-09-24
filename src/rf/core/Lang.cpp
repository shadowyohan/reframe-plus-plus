#include "rf/core/Lang.h"

#include <atomic>
#include <unordered_map>

namespace rf {
namespace {

struct Phrase {
    const char* ru;
    const char* en;
};

constexpr Phrase kEnglish[] = {
    {"reframe-hook64.dll не найдена рядом с программой ({})",
     "reframe-hook64.dll is missing next to the program ({})"},
    {"нет доступа к процессу - запустите reframe++ от имени администратора",
     "no access to the process - run reframe++ as administrator"},
    {"32-битные процессы пока не поддерживаются игровым захватом",
     "game capture does not support 32-bit processes yet"},
    {"нет подходящего окна на переднем плане", "no suitable window in the foreground"},
    {"приложение не ответило на внедрение (античит?)",
     "the application did not answer the injection (anti-cheat?)"},
    {"приложение не отдало ни одного кадра через swapchain",
     "the application never handed over a frame through its swapchain"},
    {"нечего записывать - нет подходящего окна", "nothing to record - no suitable window"},
    {"не удалось воспроизвести файл", "the file could not be played"},
    {"{:.1f} ГБ", "{:.1f} GB"},
    {"{} ГБ", "{} GB"},
    {"{:.0f} МБ", "{:.0f} MB"},
    {"{:.1f} с", "{:.1f} s"},
    {"Использовано {} из {}", "{} of {} used"},
    {"Назад", "Back"},
    {"Запись", "Recording"},
    {"галерея", "gallery"},
    {"Записей пока нет", "Nothing recorded yet"},
    {"запись", "recording"},
    {"Идёт запись", "Recording now"},
    {"Остановить запись", "Stop recording"},
    {"Активировать запись", "Start recording"},
    {"Откаты", "Instant replay"},
    {"Мгновенные повторы", "Instant replays"},
    {"Настройки", "Settings"},
    {"Открыть настройки reframe++", "Open the reframe++ settings"},
    {"настройки", "settings"},
    {"Видео", "Video"},
    {"Настройки записи видео", "Video recording settings"},
    {"Аудио", "Audio"},
    {"Настройки записи аудио", "Audio recording settings"},
    {"Диск", "Disk"},
    {"Настройки хранилища", "Disk space settings"},
    {"Горячие клавиши", "Hotkeys"},
    {"Настройки кейбиндов", "Keybinding settings"},
    {"Интерфейс", "Interface"},
    {"Настройки интерфейса", "Interface settings"},
    {"интерфейс", "interface"},
    {"значки", "badges"},
    {"Язык", "Language"},
    {"Язык интерфейса", "Interface language"},
    {"Размер интерфейса", "Interface size"},
    {"Масштаб меню и подсказок", "Menu and overlay scale"},
    {"видео", "video"},
    {"Запись рабочего стола", "Record the desktop"},
    {"Захватывать весь экран", "Capture the whole screen"},
    {"Рисовать курсор", "Draw the cursor"},
    {"Указатель мыши в записи", "Mouse pointer in the recording"},
    {"Длительность повтора", "Replay length"},
    {"Можно записать до 20 мин.", "Up to 20 minutes"},
    {"Следовать за курсором", "Follow the cursor"},
    {"Экран меняется за курсором", "The screen follows the pointer"},
    {"Задержка переключения", "Switch delay"},
    {"Пауза перед сменой экрана", "Pause before changing screens"},
    {"сразу", "at once"},
    {"Монитор", "Monitor"},
    {"Выбирается автоматически", "Chosen automatically"},
    {"Видеокарта", "Graphics card"},
    {"формат вывода", "output format"},
    {"Запись в 120+ FPS может сказаться на производительности!",
     "Recording at 120+ FPS may cost you performance!"},
    {"При включенной \"Записи рабочего стола\" FPS не может ",
     "With \"Record the desktop\" on, the frame rate cannot "},
    {"привышать герцовку вашего монитора!", "go above your monitor refresh rate!"},
    {"Качество", "Quality"},
    {"Разрешение", "Resolution"},
    {"Частота кадров", "Frame rate"},
    {"Скорость передачи", "Bitrate"},
    {"Битрейт видеопотока, Мбит/с", "Video bitrate, Mbps"},
    {"аудио", "audio"},
    {"системные звуки", "system sound"},
    {"Записывать", "Record"},
    {"Звук системы в записи", "System sound in the recording"},
    {"Громкость", "Volume"},
    {"Звук игры и приложений", "Game and application sound"},
    {"микрофон", "microphone"},
    {"Голос в записи", "Your voice in the recording"},
    {"Источник", "Source"},
    {"Системный по умолчанию", "System default"},
    {"Уровень входного сигнала", "Input level"},
    {"Усиление", "Boost"},
    {"Дополнительный буст голоса", "Extra gain for your voice"},
    {"аудиодорожки", "audio tracks"},
    {"Формат", "Layout"},
    {"Звук системы и микрофона", "System and microphone"},
    {"на одной дорожке", "on one track"},
    {"Общая, система", "A full mix, system"},
    {"и микрофон отдельно", "and microphone apart"},
    {"Общая, микрофон и", "A full mix, microphone"},
    {"каждое приложение", "and every app"},
    {"Дорожка микрофона появится только с включённым микрофоном!",
     "The microphone track only appears with the microphone on!"},
    {"горячие клавиши", "hotkeys"},
    {"Открыть Reframe++", "Open Reframe++"},
    {"Сохранить откат", "Save a replay"},
    {"Начать/остановить запись", "Start / stop recording"},
    {"Жду клавишу...", "Waiting for a key..."},
    {"Занято", "Taken"},
    {"диск", "disk"},
    {"Ограничение места", "Space limit"},
    {"Лимит на записи", "Cap on recordings"},
    {"Размер хранилища", "Storage size"},
    {"расположение", "location"},
    {"Временные файлы", "Temporary files"},
    {"Хранить откат в ОЗУ", "Keep the replay in RAM"},
    {"Без записи на диск, быстрее", "No disk writes, faster"},
    {"Галерея", "Gallery"},
    {"Здесь появятся ваши записи и мгновенные повторы",
     "Your recordings and instant replays will show up here"},
    {"Низкое", "Low"},
    {"Среднее", "Medium"},
    {"Высокое", "High"},
    {"Своё", "Custom"},
    {"Экран", "Screen"},
    {"Одна дорожка", "One track"},
    {"Раздельно", "Separate"},
    {"По приложениям", "Per app"},
    {"Звук системы и микрофон", "System and microphone"},
    {"Звук системы", "System sound"},
    {"Микрофон", "Microphone"},
    {"Количество дорожек", "Track count"},
    {"Лишние приложения", "Apps beyond the limit"},
    {"слышны в общей дорожке", "are heard in the main track"},
    {"Дорожки приложений пишутся только вместе со звуком системы!",
     "App tracks are recorded only together with system sound!"},
    {"Шумоподавление", "Noise suppression"},
    {"Шумоподавление микрофона", "Microphone noise suppression"},
    {"Режим шумоподавления", "Noise suppression mode"},
    {"Прежде чем использовать NVIDIA Maxine", "Before using NVIDIA Maxine"},
    {"требуется его загрузить", "it has to be downloaded"},
    {"Загрузить", "Download"},
    {"Загрузка...", "Downloading..."},
    {"Скачиваю NVIDIA Maxine", "Downloading NVIDIA Maxine"},
    {"Устанавливаю NVIDIA Maxine", "Installing NVIDIA Maxine"},
    {"Достигнут лимит дорожек.", "Track limit reached."},
    {"Звук из {} не записывается на свою дорожку.", "Sound from {} is not recorded on its own track."},
    {"NVIDIA Maxine работает только на видеокартах RTX",
     "NVIDIA Maxine works only on RTX graphics cards"},
    {"Не удалось установить NVIDIA Maxine", "NVIDIA Maxine could not be installed"},
    {"Не удалось переключить видеокарту", "The graphics card could not be switched"},
    {"Не удалось применить настройки", "The settings could not be applied"},
    {" (основной)", " (primary)"},
    {"Запись остановлена", "Recording stopped"},
    {"Запись запущена", "Recording started"},
    {"Запись {} запущена", "Recording {} started"},
    {"Не удалось начать запись", "Recording could not start"},
    {"Мгновенный повтор сохранен", "Instant replay saved"},
    {"Мгновенный повтор из {} сохранен", "Instant replay from {} saved"},
    {"Повтор недоступен", "Replay unavailable"},
    {"Откаты недоступны", "Instant replay unavailable"},
    {"Мгновенный повтор включен", "Instant replay on"},
    {"Мгновенный повтор выключен", "Instant replay off"},
    {"Драйвер перезапустился — запись восстановлена",
     "The display driver restarted - recording recovered"},
    {"Открыть reframe++\tAlt+Z", "Open reframe++\tAlt+Z"},
    {"Папка с записями", "Recordings folder"},
    {"Выход", "Quit"},
    {"Авто", "Auto"},
    {"Индикатор записи", "Recording indicator"},
    {"Кружок и таймер поверх игры", "Dot and timer over the game"},
    {"Кнопка остановки", "Stop button"},
    {"Рядом с индикатором записи", "Next to the timer"},
    {"Индикатор микрофона", "Microphone indicator"},
    {"Значок в углу экрана", "Badge in the screen corner"},
    {"Индикатор повтора", "Replay indicator"},
    {"Виден, пока повтор заряжен", "Badge while replay is armed"},
    {"Расположение", "Placement"},
    {"Угол экрана для значков", "Corner for the badges"},
    {"Размер значков", "Badge size"},
    {"От 60% до 200%", "From 60% to 200%"},
    {"Прозрачность", "Opacity"},
    {"Насколько значки видно", "How visible badges are"},
    {"Слева сверху", "Top left"},
    {"Справа сверху", "Top right"},
    {"Слева снизу", "Bottom left"},
    {"Справа снизу", "Bottom right"},
    {"Примерно {} на клип", "About {} per clip"},
};

std::atomic<Language> g_language{Language::Russian};

const std::unordered_map<std::string_view, const char*>& EnglishTable() {
    static const std::unordered_map<std::string_view, const char*> table = [] {
        std::unordered_map<std::string_view, const char*> map;
        map.reserve(std::size(kEnglish) * 2);
        for (const Phrase& phrase : kEnglish) map.emplace(phrase.ru, phrase.en);
        return map;
    }();
    return table;
}

}

void SetLanguage(Language language) { g_language.store(language, std::memory_order_relaxed); }

Language CurrentLanguage() { return g_language.load(std::memory_order_relaxed); }

const char* Tr(const char* russian) {
    if (!russian || g_language.load(std::memory_order_relaxed) == Language::Russian) return russian;

    const auto& table = EnglishTable();
    const auto found = table.find(std::string_view(russian));
    return found == table.end() ? russian : found->second;
}

std::string_view Tr(std::string_view russian) {
    if (g_language.load(std::memory_order_relaxed) == Language::Russian) return russian;

    const auto& table = EnglishTable();
    const auto found = table.find(russian);
    return found == table.end() ? russian : std::string_view(found->second);
}

}
