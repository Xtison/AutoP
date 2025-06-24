#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE

#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")

#include <windows.h>
#include <chrono>
#include <thread>
#include <string>
#include <sstream>
#include <iostream>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <conio.h>
#include <shellapi.h>

using namespace std;
using namespace chrono;

// Функция проверки прав администратора
bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID AdministratorsGroup;

    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &AdministratorsGroup)) {
        if (!CheckTokenMembership(NULL, AdministratorsGroup, &isAdmin)) {
            isAdmin = FALSE;
        }
        FreeSid(AdministratorsGroup);
    }
    return isAdmin != FALSE;
}

// Функция перезапуска с правами администратора
void RestartAsAdmin() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    SHELLEXECUTEINFO sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.hwnd = NULL;
    sei.nShow = SW_NORMAL;

    ShellExecuteEx(&sei);
}

// Глобальные переменные
bool isRunning = false;
bool isFirstStart = true;
int pressCount = 0;
steady_clock::time_point programStartTime;
steady_clock::time_point lastResumeTime;
steady_clock::duration totalActiveTime = seconds(0);
HWND hConsole = NULL;
HWND hStartBtn, hPauseBtn, hStatBtn, hDebugBtn, hInfoText;
HANDLE hConsoleInputThread = NULL;
bool consoleThreadRunning = false;
mutex dataMutex;

// Прототипы функций
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreateControls(HWND hwnd);
void ToggleConsole();
string formatDuration(seconds duration);
DWORD WINAPI EmulationThread(LPVOID lpParam);
DWORD WINAPI ConsoleInputThread(LPVOID lpParam);
void HandleCommand(const string& command, bool fromConsole);

// Точка входа для Windows приложения
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Проверка прав администратора
    if (!IsRunAsAdmin()) {
        int result = MessageBoxW(NULL,
            L"Для полной функциональности рекомендуется запустить программу с правами администратора.\nПерезапустить программу с правами администратора?",
            L"Права администратора",
            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);

        if (result == IDYES) {
            RestartAsAdmin();
            return 0; // Завершаем текущий экземпляр
        }
    }

    // Регистрация класса окна
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"KeyEmulatorClass";

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Ошибка регистрации окна!", L"Ошибка", MB_ICONERROR);
        return 0;
    }

    // Создание основного окна (уменьшенная ширина)
    HWND hwnd = CreateWindowW(
        L"KeyEmulatorClass",
        L"Auto Pidor 2.0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        240, 280, // Уменьшенная ширина, увеличенная высота
        NULL, NULL, hInstance, NULL);

    if (hwnd == NULL) {
        MessageBoxW(NULL, L"Ошибка создания окна!", L"Ошибка", MB_ICONERROR);
        return 0;
    }

    // Создаем элементы управления
    CreateControls(hwnd);

    // Отображаем окно
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Запускаем поток эмуляции
    CreateThread(NULL, 0, EmulationThread, NULL, 0, NULL);

    // Цикл обработки сообщений
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Очистка ресурсов
    consoleThreadRunning = false;
    if (hConsoleInputThread) {
        // Отправляем символ в консоль, чтобы разблокировать ввод
        if (hConsole) {
            PostMessage(hConsole, WM_CHAR, VK_RETURN, 0);
        }
        WaitForSingleObject(hConsoleInputThread, 1000);
        CloseHandle(hConsoleInputThread);
    }

    return (int)msg.wParam;
}

// Форматирование времени в строку (часы:минуты:секунды) - английский
string formatDuration(seconds duration) {
    auto h = duration_cast<hours>(duration);
    duration -= h;
    auto m = duration_cast<minutes>(duration);
    duration -= m;
    auto s = duration_cast<seconds>(duration);

    stringstream ss;
    if (h.count() > 0) ss << h.count() << "h ";
    if (m.count() > 0 || h.count() > 0) ss << m.count() << "m ";
    ss << s.count() << "s";
    return ss.str();
}

// Оконная процедура (обработка сообщений)
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        // Обработка нажатий кнопок
        if ((HWND)lParam == hStartBtn) {
            lock_guard<mutex> lock(dataMutex);
            if (isFirstStart) {
                programStartTime = steady_clock::now();
                lastResumeTime = programStartTime;
                isFirstStart = false;
            }
            else {
                lastResumeTime = steady_clock::now();
            }
            isRunning = true;
            MessageBoxW(hwnd, L"Запуск работы цикла.", L"Запуск", MB_OK);
        }
        else if ((HWND)lParam == hPauseBtn) {
            lock_guard<mutex> lock(dataMutex);
            if (isRunning) {
                auto now = steady_clock::now();
                totalActiveTime += (now - lastResumeTime);
                isRunning = false;
                MessageBoxW(hwnd, L"Завершение работы цикла.", L"Пауза", MB_OK);
            }
        }
        else if ((HWND)lParam == hStatBtn) {
            lock_guard<mutex> lock(dataMutex);
            steady_clock::duration elapsedTime = totalActiveTime;

            // Если эмуляция активна, добавляем текущее время работы
            if (isRunning) {
                auto now = steady_clock::now();
                elapsedTime += (now - lastResumeTime);
            }

            wstringstream wss;
            wss << L"Статистика:\n";
            wss << L"Время работы: " << formatDuration(duration_cast<seconds>(elapsedTime)).c_str() << L"\n";
            wss << L"Нажатий F: " << pressCount;

            MessageBoxW(hwnd, wss.str().c_str(), L"Статистика", MB_OK);
        }
        else if ((HWND)lParam == hDebugBtn) {
            ToggleConsole();
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Создание элементов управления (центрированные кнопки)
void CreateControls(HWND hwnd) {
    const int BUTTON_WIDTH = 200;  // Ширина кнопок
    const int BUTTON_HEIGHT = 35;  // Высота кнопок
    const int MARGIN = 10;         // Отступы
    const int START_Y = 60;        // Начальная позиция по Y

    // Информационная надпись вверху
    hInfoText = CreateWindowW(L"STATIC",
        L"Software by Xtison\nAutoPidor V 2.0",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        MARGIN, MARGIN,
        BUTTON_WIDTH, 37,
        hwnd, NULL, NULL, NULL);

    // Позиции кнопок
    int y = START_Y;

    // Кнопка Старт
    hStartBtn = CreateWindowW(L"BUTTON", L"▶ Старт",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        MARGIN, y, BUTTON_WIDTH, BUTTON_HEIGHT,
        hwnd, (HMENU)1, NULL, NULL);
    y += BUTTON_HEIGHT + MARGIN;

    // Кнопка Пауза
    hPauseBtn = CreateWindowW(L"BUTTON", L"Пауза",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        MARGIN, y, BUTTON_WIDTH, BUTTON_HEIGHT,
        hwnd, (HMENU)2, NULL, NULL);
    y += BUTTON_HEIGHT + MARGIN;

    // Кнопка Статистика
    hStatBtn = CreateWindowW(L"BUTTON", L"Статистика",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        MARGIN, y, BUTTON_WIDTH, BUTTON_HEIGHT,
        hwnd, (HMENU)3, NULL, NULL);
    y += BUTTON_HEIGHT + MARGIN;

    // Кнопка Консоль отладки
    hDebugBtn = CreateWindowW(L"BUTTON", L"Консоль отладки",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        MARGIN, y, BUTTON_WIDTH, BUTTON_HEIGHT,
        hwnd, (HMENU)4, NULL, NULL);
}

// Переключение консоли отладки
void ToggleConsole() {
    if (hConsole == NULL) {
        AllocConsole();
        hConsole = GetConsoleWindow();

        // Перенаправляем стандартные потоки
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);

        // Настраиваем буферизацию
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        setvbuf(stdin, NULL, _IONBF, 0);

        // Запускаем поток для чтения команд
        consoleThreadRunning = true;
        hConsoleInputThread = CreateThread(NULL, 0, ConsoleInputThread, NULL, 0, NULL);

        // Английские сообщения в консоли
        cout << "Debug console activated" << endl;
        cout << "Type 'help' to see all commands" << endl;
        cout << "> " << flush;
    }
    else {
        consoleThreadRunning = false;

        // Отправляем Enter, чтобы разблокировать поток ввода
        if (hConsole) {
            PostMessage(hConsole, WM_CHAR, VK_RETURN, 0);
        }

        if (hConsoleInputThread) {
            WaitForSingleObject(hConsoleInputThread, 1000);
            CloseHandle(hConsoleInputThread);
            hConsoleInputThread = NULL;
        }

        FreeConsole();
        hConsole = NULL;
    }
}

// Поток эмуляции нажатий клавиши F
DWORD WINAPI EmulationThread(LPVOID lpParam) {
    while (true) {
        {
            lock_guard<mutex> lock(dataMutex);
            if (isRunning && (GetAsyncKeyState(0x46) & 0x8000)) {
                INPUT input;
                input.type = INPUT_KEYBOARD;
                input.ki.wScan = 0;
                input.ki.time = 0;
                input.ki.dwExtraInfo = 0;

                // Нажатие F
                input.ki.wVk = 0x46;
                input.ki.dwFlags = 0;
                SendInput(1, &input, sizeof(INPUT));

                // Отжатие F
                input.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(INPUT));

                pressCount++;

                // Вывод в консоль отладки (английский)
                if (hConsole != NULL) {
                    cout << "F key pressed (total: " << pressCount << ")" << endl;
                }
            }
        }
        Sleep(100);
    }
    return 0;
}

// Поток обработки ввода в консоли отладки
DWORD WINAPI ConsoleInputThread(LPVOID lpParam) {
    string inputLine;

    while (consoleThreadRunning) {
        // Читаем всю строку целиком
        cout << "> " << flush;

        string command;
        if (getline(cin, command)) {
            // Проверяем, не нужно ли завершить поток
            if (!consoleThreadRunning) break;

            // Обрабатываем команду
            if (!command.empty()) {
                HandleCommand(command, true);
            }
        }
        else if (!consoleThreadRunning) {
            break;
        }
        else {
            // Сброс состояния ошибки ввода
            cin.clear();
            Sleep(50);
        }
    }

    return 0;
}

// Обработка команд (английские ответы в консоли)
void HandleCommand(const string& command, bool fromConsole) {
    if (command == "stop") {
        lock_guard<mutex> lock(dataMutex);
        if (isRunning) {
            auto now = steady_clock::now();
            totalActiveTime += (now - lastResumeTime);
            isRunning = false;
        }
        if (fromConsole) {
            cout << "Emulation paused" << endl;
        }
    }
    else if (command == "continue") {
        lock_guard<mutex> lock(dataMutex);
        if (!isRunning) {
            lastResumeTime = steady_clock::now();
            isRunning = true;
        }
        if (fromConsole) {
            cout << "Emulation resumed" << endl;
        }
    }
    else if (command == "stat") {
        lock_guard<mutex> lock(dataMutex);
        steady_clock::duration elapsedTime = totalActiveTime;

        if (isRunning) {
            auto now = steady_clock::now();
            elapsedTime += (now - lastResumeTime);
        }

        if (fromConsole) {
            auto secondsTime = duration_cast<seconds>(elapsedTime);
            cout << "Statistics:" << endl;
            cout << "  Running time: " << formatDuration(secondsTime) << endl;
            cout << "  F key presses: " << pressCount << endl;
        }
    }
    else if (command == "1488") {
        if (fromConsole) {
            cout << "...............???..............." << endl; 
            Sleep(100);
            cout << ".............??...??............." << endl; 
            Sleep(100);
            cout << "...........??....???............." << endl; 
            Sleep(100);
            cout << ".........??....??.??.???........." << endl; 
            Sleep(100);
            cout << ".......??....??.??.??...??......." << endl; 
            Sleep(100);
            cout << ".......???....??.??.......??....." << endl; 
            Sleep(100);
            cout << "...???.??.??....?....???....??..." << endl; 
            Sleep(100);
            cout << ".??...??.??.??.....??.?.??....??." << endl; 
            Sleep(100);
            cout << ".???....??.??.......??.??.??.???." << endl; 
            Sleep(100);
            cout << ".??.??....?....???....??.??.?.??." << endl; 
            Sleep(100);
            cout << "...??.??.....??.?.??....??.???..." << endl; 
            Sleep(100);
            cout << ".....??.??.??.??.??....???......." << endl; 
            Sleep(100);
            cout << ".......??.?.??.??....??.??......." << endl; 
            Sleep(100);
            cout << ".............???.??.??..........." << endl; 
            Sleep(100);
            cout << ".............??.?.??............." << endl; 
            Sleep(100);
            cout << "...............???..............." << endl;
        }
    }
    else if (command == "80085") {
        if (fromConsole) {
            cout << "(.)(.)" << endl;
        }
    }
    else if (command == "help") {
        if (fromConsole) {
            cout << "Available commands: stop, continue, stat" << endl;
        }
    }
    else {
        if (fromConsole) {
            cout << "Unknown command. Type 'help' to see all commands" << endl;
        }
    }
}