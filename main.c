#include "efi.h"

#define LINE_MAX 128

/* Текущий цвет текста */
static UINTN g_color = 0x0F;

/* Время загрузки */
static EFI_TIME g_boot_time;
static BOOLEAN  g_have_boot_time = FALSE;


/* ============================================================
 * RAM filesystem
 * ============================================================ */

#define FS_MAX_FILES   32
#define FS_NAME_MAX    32
#define FS_DATA_MAX    2048

typedef struct {
    BOOLEAN used;
    CHAR16  name[FS_NAME_MAX];
    CHAR16  data[FS_DATA_MAX];
    UINTN   size;
} FS_FILE;

static FS_FILE g_fs[FS_MAX_FILES];


/* ============================================================
 * Command history
 * ============================================================ */

#define HIST_MAX 16

static CHAR16 g_history[HIST_MAX][LINE_MAX];
static UINTN  g_history_count = 0;


/* ============================================================
 * Terminal scrollback
 *
 * TERMINAL_ROWS = физическая высота обычного UEFI
 * text mode. Обычно OVMF использует 80x25.
 *
 * Последняя строка оставляется под prompt.
 * ============================================================ */

#define SCROLLBACK_MAX_LINES     256
#define SCROLLBACK_LINE_MAX      128
#define TERMINAL_ROWS            25
#define SCROLLBACK_VISIBLE_ROWS  24

static CHAR16 g_scrollback[
    SCROLLBACK_MAX_LINES
][SCROLLBACK_LINE_MAX];

static UINTN g_scrollback_count = 0;
static UINTN g_scrollback_line_len = 0;

/*
 * 0 = самый низ
 * 1 = на один экран вверх
 * 2 = ещё выше
 */
static int g_scrollback_view = 0;

/*
 * TRUE, когда мы просто перерисовываем
 * уже существующий scrollback.
 *
 * В этот момент новые символы в историю
 * записывать нельзя.
 */
static BOOLEAN g_scrollback_replaying = FALSE;


/* ============================================================
 * Scrollback internals
 * ============================================================ */

static void scrollback_newline(void)
{
    UINTN index =
        g_scrollback_count % SCROLLBACK_MAX_LINES;

    g_scrollback[index][g_scrollback_line_len] = 0;

    g_scrollback_count++;
    g_scrollback_line_len = 0;
}


static void scrollback_char(CHAR16 c)
{
    if (g_scrollback_replaying)
        return;

    if (c == L'\r')
        return;

    if (c == L'\n') {
        scrollback_newline();
        return;
    }

    /*
     * Управляющие символы не записываем.
     */
    if (c < 32)
        return;

    if (g_scrollback_line_len <
        SCROLLBACK_LINE_MAX - 1) {

        UINTN index =
            g_scrollback_count %
            SCROLLBACK_MAX_LINES;

        g_scrollback[index][g_scrollback_line_len++] = c;
    }
}


/*
 * Перерисовать prompt + текущую строку ввода.
 *
 * ВАЖНО:
 * здесь НЕ используется print(), иначе prompt
 * снова попадёт в scrollback.
 */
static void redraw_input(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    const CHAR16 *line
)
{
    CHAR16 prompt[] = L"> ";

    out->OutputString(out, prompt);

    if (line)
        out->OutputString(out, (CHAR16 *)line);
}


/* ============================================================
 * Scrollback renderer
 *
 * view = 0:
 *     последние строки
 *
 * view > 0:
 *     страницы выше
 * ============================================================ */

static void scrollback_render(
    EFI_SYSTEM_TABLE *st,
    int view
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (g_scrollback_count == 0)
        return;

    /*
     * Сколько строк реально хранится.
     */
    UINTN total =
        (g_scrollback_count < SCROLLBACK_MAX_LINES)
        ? g_scrollback_count
        : SCROLLBACK_MAX_LINES;

    /*
     * Сколько строк истории помещается,
     * потому что последнюю строку оставляем
     * под prompt.
     */
    UINTN visible =
        SCROLLBACK_VISIBLE_ROWS;

    int max_view;

    if (total > visible)
        max_view =
            (int)total - (int)visible;
    else
        max_view = 0;

    if (view < 0)
        view = 0;

    if (view > max_view)
        view = max_view;

    g_scrollback_view = view;


    /*
     * Самая старая строка ring-buffer.
     */
    UINTN oldest;

    if (g_scrollback_count <=
        SCROLLBACK_MAX_LINES) {

        oldest = 0;

    } else {

        oldest =
            g_scrollback_count %
            SCROLLBACK_MAX_LINES;
    }


    /*
     * Вычисляем начало страницы ОТ КОНЦА.
     *
     * view = 0:
     *
     *     [........][последние 24 строки]
     *
     * view = 1:
     *
     *     [.......][24 строки перед последними]
     */
    int start_offset =
        (int)total -
        (int)visible -
        view;

    if (start_offset < 0)
        start_offset = 0;


    UINTN start =
        (oldest + (UINTN)start_offset)
        % SCROLLBACK_MAX_LINES;


    /*
     * Теперь выводим историю.
     */
    g_scrollback_replaying = TRUE;

    out->ClearScreen(out);


    for (UINTN row = 0;
         row < visible;
         row++) {

        UINTN logical =
            (UINTN)start_offset + row;

        if (logical >= total)
            break;


        UINTN index =
            (start + row)
            % SCROLLBACK_MAX_LINES;


        out->OutputString(
            out,
            g_scrollback[index]
        );


        /*
         * UEFI лучше явно получать CRLF.
         */
        CHAR16 nl[2];

        nl[0] = L'\r';
        nl[1] = 0;

        out->OutputString(out, nl);

        nl[0] = L'\n';

        out->OutputString(out, nl);
    }


    g_scrollback_replaying = FALSE;
}


/* ============================================================
 * Output helpers
 * ============================================================ */

static void print(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    const char *s
)
{
    CHAR16 buf[2];

    buf[1] = 0;

    while (*s) {

        if (*s == '\n') {

            scrollback_char(L'\n');

            buf[0] = L'\r';
            out->OutputString(out, buf);

            buf[0] = L'\n';
            out->OutputString(out, buf);

        } else {

            CHAR16 c =
                (CHAR16)(unsigned char)*s;

            scrollback_char(c);

            buf[0] = c;

            out->OutputString(
                out,
                buf
            );
        }

        s++;
    }

    /*
     * Если мы не перерисовываем scrollback,
     * новый вывод возвращает нас вниз.
     */
    if (!g_scrollback_replaying)
        g_scrollback_view = 0;
}


/*
 * Вывод CHAR16 строки.
 *
 * Нужен для:
 *   - UEFI строк
 *   - файлов
 *   - пользовательского ввода
 *   - FirmwareVendor
 */
static void print16(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    const CHAR16 *s
)
{
    if (!s) {
        print(out, "?");
        return;
    }

    while (*s) {

        scrollback_char(*s);

        CHAR16 buf[2];

        buf[0] = *s;
        buf[1] = 0;

        out->OutputString(
            out,
            buf
        );

        s++;
    }

    if (!g_scrollback_replaying)
        g_scrollback_view = 0;
}


/*
 * Вывод беззнакового числа в десятичном виде.
 *
 * ВАЖНО: раньше эта функция вызывалась по всему файлу
 * (cmd_ls, cmd_touch, cmd_fetch, calc, ...), но нигде
 * не была определена - это ошибка компиляции
 * (implicit declaration of function). Добавлена здесь,
 * как можно раньше, чтобы быть видимой для всех
 * последующих вызовов в файле.
 */
static void print_uint(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    UINT64 value
)
{
    CHAR16 digits[21];
    CHAR16 buf[21];
    UINTN  n = 0;

    if (value == 0) {
        print(out, "0");
        return;
    }

    while (value > 0 && n < 20) {
        digits[n++] = (CHAR16)(L'0' + (value % 10));
        value /= 10;
    }

    for (UINTN i = 0; i < n; i++)
        buf[i] = digits[n - 1 - i];

    buf[n] = 0;

    print16(out, buf);
}


/*
 * То же самое, но всегда минимум 2 цифры
 * (с ведущим нулём) - используется для часов/минут/секунд.
 * Тоже отсутствовала в исходнике - вторая недостающая
 * функция, вызывавшая ошибку компиляции.
 */
static void print_uint2(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    UINTN value
)
{
    if (value < 10)
        print(out, "0");

    print_uint(out, (UINT64)value);
}


/* ============================================================
 * Basic string helpers
 * ============================================================ */

static UINTN char16_len(const CHAR16 *s)
{
    UINTN n = 0;

    while (s[n])
        n++;

    return n;
}


static void char16_copy(
    CHAR16 *dst,
    const CHAR16 *src,
    UINTN max
)
{
    UINTN i = 0;

    if (max == 0)
        return;

    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}


static int char16_eq(
    const CHAR16 *a,
    const CHAR16 *b
)
{
    while (*a && *b) {

        if (*a != *b)
            return 0;

        a++;
        b++;
    }

    return *a == 0 && *b == 0;
}


/* ============================================================
 * Parsing helpers
 * ============================================================ */

static int streq(
    const CHAR16 *a,
    const char *b
)
{
    while (*a && *b) {

        if (*a !=
            (CHAR16)(unsigned char)*b)
            return 0;

        a++;
        b++;
    }

    return *a == 0 && *b == 0;
}


static int starts_with(
    const CHAR16 *a,
    const char *prefix
)
{
    while (*prefix) {

        if (*a !=
            (CHAR16)(unsigned char)*prefix)
            return 0;

        a++;
        prefix++;
    }

    return 1;
}


static UINTN parse_uint(
    const CHAR16 *s
)
{
    while (*s == L' ')
        s++;

    UINTN v = 0;

    while (*s >= L'0' &&
           *s <= L'9') {

        v =
            v * 10 +
            (UINTN)(*s - L'0');

        s++;
    }

    return v;
}


static CHAR16 *skip_ws16(
    CHAR16 *s
)
{
    while (*s == L' ')
        s++;

    return s;
}


static CHAR16 *take_word(
    CHAR16 *s,
    CHAR16 *out,
    UINTN max
)
{
    UINTN i = 0;

    if (max == 0)
        return s;

    while (*s &&
           *s != L' ' &&
           i < max - 1) {

        out[i++] = *s++;
    }

    out[i] = 0;

    return s;
}


/* ============================================================
 * Input
 * ============================================================ */

static void erase_input_line(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    UINTN len
)
{
    for (UINTN i = 0; i < len; i++)
        print(out, "\b \b");
}


/*
 * Чтение строки.
 *
 * PageUp    = scrollback вверх
 * PageDown  = scrollback вниз
 * Home      = самый верх истории
 * End       = самый низ
 *
 * Up/Down   = command history
 */
static void read_line(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *line,
    UINTN max
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    SIMPLE_INPUT_INTERFACE *in =
        st->ConIn;

    UINTN len = 0;

    int history_pos = -1;

    line[0] = 0;


    for (;;) {

        EFI_INPUT_KEY key;

        EFI_STATUS status =
            in->ReadKeyStroke(
                in,
                &key
            );


        if (status != EFI_SUCCESS) {

            st->BootServices->Stall(
                10000
            );

            continue;
        }


        /* ====================================================
         * ENTER
         * ==================================================== */

        if (key.UnicodeChar ==
            CHAR_CARRIAGE_RETURN) {

            line[len] = 0;

            print(out, "\n");

            return;
        }


        /* ====================================================
         * BACKSPACE
         * ==================================================== */

        if (key.UnicodeChar ==
            CHAR_BACKSPACE) {

            if (len > 0) {

                len--;

                line[len] = 0;

                print(out, "\b \b");
            }

            continue;
        }


        /* ====================================================
         * SPECIAL KEYS
         * ==================================================== */

        if (key.UnicodeChar == 0) {


            /* =================================================
             * PAGE UP
             * ScanCode 0x09
             * ================================================= */

            if (key.ScanCode == 0x09) {

                int step =
                    SCROLLBACK_VISIBLE_ROWS - 1;

                if (step < 1)
                    step = 1;

                int new_view =
                    g_scrollback_view + step;

                scrollback_render(
                    st,
                    new_view
                );

                redraw_input(
                    out,
                    line
                );

                continue;
            }


            /* =================================================
             * PAGE DOWN
             * ScanCode 0x0A
             * ================================================= */

            if (key.ScanCode == 0x0A) {

                int step =
                    SCROLLBACK_VISIBLE_ROWS - 1;

                if (step < 1)
                    step = 1;

                int new_view =
                    g_scrollback_view - step;

                if (new_view < 0)
                    new_view = 0;

                scrollback_render(
                    st,
                    new_view
                );

                redraw_input(
                    out,
                    line
                );

                continue;
            }


            /* =================================================
             * HOME
             * ScanCode 0x05
             * ================================================= */

            if (key.ScanCode == 0x05) {

                scrollback_render(
                    st,
                    999999
                );

                redraw_input(
                    out,
                    line
                );

                continue;
            }


            /* =================================================
             * END
             * ScanCode 0x06
             * ================================================= */

            if (key.ScanCode == 0x06) {

                scrollback_render(
                    st,
                    0
                );

                redraw_input(
                    out,
                    line
                );

                continue;
            }


            /* =================================================
             * UP — command history
             * ScanCode 1
             * ================================================= */

            if (key.ScanCode == 1) {

                if (g_history_count > 0) {

                    if (history_pos == -1) {

                        history_pos =
                            (int)g_history_count - 1;

                    } else if (history_pos > 0) {

                        history_pos--;
                    }


                    /*
                     * Стираем текущий ввод.
                     */
                    for (UINTN i = 0;
                         i < len;
                         i++) {

                        print(
                            out,
                            "\b \b"
                        );
                    }


                    UINTN index;


                    if (g_history_count <=
                        HIST_MAX) {

                        index =
                            (UINTN)history_pos;

                    } else {

                        UINTN start =
                            g_history_count %
                            HIST_MAX;

                        index =
                            (start +
                             (UINTN)history_pos)
                            % HIST_MAX;
                    }


                    char16_copy(
                        line,
                        g_history[index],
                        max
                    );

                    len =
                        char16_len(line);


                    print16(
                        out,
                        line
                    );
                }

                continue;
            }


            /* =================================================
             * DOWN — command history
             * ScanCode 2
             * ================================================= */

            if (key.ScanCode == 2) {

                if (history_pos != -1) {

                    for (UINTN i = 0;
                         i < len;
                         i++) {

                        print(
                            out,
                            "\b \b"
                        );
                    }


                    history_pos++;


                    if (history_pos >=
                            (int)g_history_count ||
                        history_pos >=
                            (int)HIST_MAX) {

                        history_pos = -1;

                        len = 0;

                        line[0] = 0;

                    } else {

                        UINTN index;


                        if (g_history_count <=
                            HIST_MAX) {

                            index =
                                (UINTN)history_pos;

                        } else {

                            UINTN start =
                                g_history_count %
                                HIST_MAX;

                            index =
                                (start +
                                 (UINTN)history_pos)
                                % HIST_MAX;
                        }


                        char16_copy(
                            line,
                            g_history[index],
                            max
                        );

                        len =
                            char16_len(line);


                        print16(
                            out,
                            line
                        );
                    }
                }

                continue;
            }


            continue;
        }


        /* ====================================================
         * NORMAL CHARACTER
         * ==================================================== */

        if (len < max - 1) {

            line[len++] =
                key.UnicodeChar;

            line[len] = 0;


            CHAR16 echo[2] = {
                key.UnicodeChar,
                0
            };


            out->OutputString(
                out,
                echo
            );


            history_pos = -1;
        }
    }
}


/* ============================================================
 * RAM filesystem
 * ============================================================ */

static int fs_find(
    const CHAR16 *name
)
{
    for (int i = 0;
         i < FS_MAX_FILES;
         i++) {

        if (g_fs[i].used &&
            char16_eq(
                g_fs[i].name,
                name
            ))
            return i;
    }

    return -1;
}


static int fs_find_free(void)
{
    for (int i = 0;
         i < FS_MAX_FILES;
         i++) {

        if (!g_fs[i].used)
            return i;
    }

    return -1;
}


static void cmd_ls(
    EFI_SYSTEM_TABLE *st
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    int any = 0;

    for (int i = 0;
         i < FS_MAX_FILES;
         i++) {

        if (!g_fs[i].used)
            continue;

        any = 1;

        print16(
            out,
            g_fs[i].name
        );

        print(out, "  (");

        print_uint(
            out,
            g_fs[i].size
        );

        print(out, " bytes)\n");
    }

    if (!any) {

        print(
            out,
            "(empty - no files. use 'touch <name>' or 'write <name> <text>')\n"
        );
    }
}


static void cmd_touch(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *name
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(name) == 0) {

        print(
            out,
            "Usage: touch <name>\n"
        );

        return;
    }


    if (fs_find(name) >= 0) {

        print(
            out,
            "File already exists.\n"
        );

        return;
    }


    int idx =
        fs_find_free();


    if (idx < 0) {

        print(
            out,
            "Filesystem full (max "
        );

        print_uint(
            out,
            FS_MAX_FILES
        );

        print(out, " files).\n");

        return;
    }


    g_fs[idx].used = TRUE;

    char16_copy(
        g_fs[idx].name,
        name,
        FS_NAME_MAX
    );

    g_fs[idx].data[0] = 0;

    g_fs[idx].size = 0;

    print(out, "Created.\n");
}


static void cmd_cat(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *name
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(name) == 0) {

        print(
            out,
            "Usage: cat <name>\n"
        );

        return;
    }


    int idx =
        fs_find(name);


    if (idx < 0) {

        print(
            out,
            "No such file.\n"
        );

        return;
    }


    if (g_fs[idx].size == 0) {

        print(
            out,
            "(empty file)\n"
        );

        return;
    }


    /*
     * Важно: print16(), а не прямой OutputString(),
     * чтобы cat попадал в scrollback.
     */
    print16(
        out,
        g_fs[idx].data
    );

    print(out, "\n");
}


static void cmd_write(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *name,
    CHAR16 *text
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(name) == 0) {

        print(
            out,
            "Usage: write <name> <text>\n"
        );

        return;
    }


    int idx =
        fs_find(name);


    if (idx < 0)
        idx = fs_find_free();


    if (idx < 0) {

        print(
            out,
            "Filesystem full.\n"
        );

        return;
    }


    g_fs[idx].used = TRUE;


    char16_copy(
        g_fs[idx].name,
        name,
        FS_NAME_MAX
    );


    char16_copy(
        g_fs[idx].data,
        text,
        FS_DATA_MAX
    );


    g_fs[idx].size =
        char16_len(
            g_fs[idx].data
        );


    print(out, "Written (");

    print_uint(
        out,
        g_fs[idx].size
    );

    print(out, " bytes).\n");
}


static void cmd_append(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *name,
    CHAR16 *text
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(name) == 0) {

        print(
            out,
            "Usage: append <name> <text>\n"
        );

        return;
    }


    int idx =
        fs_find(name);


    if (idx < 0)
        idx = fs_find_free();


    if (idx < 0) {

        print(
            out,
            "Filesystem full.\n"
        );

        return;
    }


    if (!g_fs[idx].used) {

        g_fs[idx].used = TRUE;

        char16_copy(
            g_fs[idx].name,
            name,
            FS_NAME_MAX
        );

        g_fs[idx].data[0] = 0;

        g_fs[idx].size = 0;
    }


    UINTN cur =
        g_fs[idx].size;

    UINTN need =
        char16_len(text);

    UINTN sep =
        (cur > 0) ? 1 : 0;


    if (cur + sep + need >=
        FS_DATA_MAX) {

        print(
            out,
            "File too large, cannot append that much.\n"
        );

        return;
    }


    if (sep)
        g_fs[idx].data[cur++] =
            L'\n';


    for (UINTN i = 0;
         i < need;
         i++) {

        g_fs[idx].data[cur++] =
            text[i];
    }


    g_fs[idx].data[cur] = 0;

    g_fs[idx].size = cur;


    print(
        out,
        "Appended ("
    );

    print_uint(
        out,
        g_fs[idx].size
    );

    print(
        out,
        " bytes total).\n"
    );
}


static void cmd_rm(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *name
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(name) == 0) {

        print(
            out,
            "Usage: rm <name>\n"
        );

        return;
    }


    int idx =
        fs_find(name);


    if (idx < 0) {

        print(
            out,
            "No such file.\n"
        );

        return;
    }


    g_fs[idx].used = FALSE;

    g_fs[idx].size = 0;


    print(
        out,
        "Deleted.\n"
    );
}


static void cmd_mv(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *oldname,
    CHAR16 *newname
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(oldname) == 0 ||
        char16_len(newname) == 0) {

        print(
            out,
            "Usage: mv <old> <new>\n"
        );

        return;
    }


    int idx =
        fs_find(oldname);


    if (idx < 0) {

        print(
            out,
            "No such file.\n"
        );

        return;
    }


    if (fs_find(newname) >= 0) {

        print(
            out,
            "Target name already exists.\n"
        );

        return;
    }


    char16_copy(
        g_fs[idx].name,
        newname,
        FS_NAME_MAX
    );


    print(
        out,
        "Renamed.\n"
    );
}


static void cmd_cp(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *src,
    CHAR16 *dst
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(src) == 0 ||
        char16_len(dst) == 0) {

        print(
            out,
            "Usage: cp <src> <dst>\n"
        );

        return;
    }


    int si =
        fs_find(src);


    if (si < 0) {

        print(
            out,
            "No such file.\n"
        );

        return;
    }


    if (fs_find(dst) >= 0) {

        print(
            out,
            "Target name already exists.\n"
        );

        return;
    }


    int di =
        fs_find_free();


    if (di < 0) {

        print(
            out,
            "Filesystem full.\n"
        );

        return;
    }


    g_fs[di].used = TRUE;


    char16_copy(
        g_fs[di].name,
        dst,
        FS_NAME_MAX
    );


    char16_copy(
        g_fs[di].data,
        g_fs[si].data,
        FS_DATA_MAX
    );


    g_fs[di].size =
        g_fs[si].size;


    print(
        out,
        "Copied.\n"
    );
}


/* ============================================================
 * Text editor
 * ============================================================ */

static void cmd_edit(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *name
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    if (char16_len(name) == 0) {

        print(
            out,
            "Usage: edit <name>\n"
        );

        return;
    }


    int idx =
        fs_find(name);


    if (idx < 0)
        idx = fs_find_free();


    if (idx < 0) {

        print(
            out,
            "Filesystem full.\n"
        );

        return;
    }


    if (!g_fs[idx].used) {

        g_fs[idx].used = TRUE;

        char16_copy(
            g_fs[idx].name,
            name,
            FS_NAME_MAX
        );
    }


    print(
        out,
        "Editing '"
    );

    print16(
        out,
        name
    );

    print(
        out,
        "'. Type lines, finish with a line containing just '.'\n"
    );


    CHAR16 buf[FS_DATA_MAX];

    UINTN pos = 0;

    buf[0] = 0;


    CHAR16 ln[LINE_MAX];


    for (;;) {

        print(
            out,
            ": "
        );


        read_line(
            st,
            ln,
            LINE_MAX
        );


        if (ln[0] == L'.' &&
            ln[1] == 0)
            break;


        UINTN llen =
            char16_len(ln);


        if (pos > 0 &&
            pos + 1 < FS_DATA_MAX) {

            buf[pos++] =
                L'\n';
        }


        for (UINTN i = 0;
             i < llen &&
             pos < FS_DATA_MAX - 1;
             i++) {

            buf[pos++] =
                ln[i];
        }


        buf[pos] = 0;


        if (pos >= FS_DATA_MAX - 1) {

            print(
                out,
                "(file full, stopping edit)\n"
            );

            break;
        }
    }


    char16_copy(
        g_fs[idx].data,
        buf,
        FS_DATA_MAX
    );


    g_fs[idx].size =
        char16_len(
            g_fs[idx].data
        );


    print(
        out,
        "Saved ("
    );

    print_uint(
        out,
        g_fs[idx].size
    );

    print(
        out,
        " bytes).\n"
    );
}


/* ============================================================
 * Calculator
 * ============================================================ */

static void cmd_calc(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *rest
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    rest =
        skip_ws16(rest);


    CHAR16 a[32];
    CHAR16 op[8];
    CHAR16 b[32];


    rest =
        take_word(
            rest,
            a,
            32
        );


    rest =
        skip_ws16(rest);


    rest =
        take_word(
            rest,
            op,
            8
        );


    rest =
        skip_ws16(rest);


    take_word(
        rest,
        b,
        32
    );


    if (char16_len(a) == 0 ||
        char16_len(op) == 0 ||
        char16_len(b) == 0) {

        print(
            out,
            "Usage: calc <a> <+|-|*|/> <b>\n"
        );

        return;
    }


    INTN x =
        (INTN)parse_uint(a);

    INTN y =
        (INTN)parse_uint(b);

    INTN r;


    if (char16_eq(op, L"+")) {

        r = x + y;

    } else if (char16_eq(op, L"-")) {

        r = x - y;

    } else if (char16_eq(op, L"*")) {

        r = x * y;

    } else if (char16_eq(op, L"/")) {

        if (y == 0) {

            print(
                out,
                "Error: division by zero.\n"
            );

            return;
        }

        r = x / y;

    } else {

        print(
            out,
            "Unknown operator. Use + - * /\n"
        );

        return;
    }


    if (r < 0) {

        print(out, "-");

        r = -r;
    }


    print_uint(
        out,
        (UINT64)r
    );

    print(
        out,
        "\n"
    );
}


/* ============================================================
 * Fetch helpers
 * ============================================================ */

static void print_label(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    const char *label
)
{
    out->SetAttribute(
        out,
        0x0B
    );

    print(
        out,
        label
    );

    out->SetAttribute(
        out,
        0x0F
    );
}


static void print_separator(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out
)
{
    out->SetAttribute(
        out,
        0x08
    );

    print(
        out,
        "----------------------------------------\n"
    );

    out->SetAttribute(
        out,
        0x0F
    );
}


static void print_bool(
    SIMPLE_TEXT_OUTPUT_INTERFACE *out,
    BOOLEAN value
)
{
    out->SetAttribute(
        out,
        value ? 0x0A : 0x0C
    );

    print(
        out,
        value ? "Yes" : "No"
    );

    out->SetAttribute(
        out,
        0x0F
    );
}


/* ============================================================
 * Fetch / neofetch
 * ============================================================ */

static void cmd_fetch(
    EFI_SYSTEM_TABLE *st
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;

    UINTN saved =
        g_color;


    /* --------------------------------------------------------
     * ASCII logo
     * -------------------------------------------------------- */

    const char *logo[] = {

        " /$$      /$$            /$$$$$$   /$$$$$$ ",
        "| $$$    /$$$           /$$__  $$ /$$__  $$",
        "| $$$$  /$$$$ /$$   /$$| $$  \\ $$| $$  \\__/",
        "| $$ $$/$$ $$| $$  | $$| $$  | $$|  $$$$$$ ",
        "| $$  $$$| $$| $$  | $$| $$  | $$ \\____  $$",
        "| $$\\  $ | $$| $$  | $$| $$  | $$ /$$  \\ $$",
        "| $$ \\/  | $$|  $$$$$$$|  $$$$$$/|  $$$$$$/",
        "|__/     |__/ \\____  $$ \\______/  \\______/ ",
        "              /$$  | $$                    ",
        "             |  $$$$$$/                    ",
        "              \\______/                     "
    };


    UINTN logo_colors[] = {

        0x0B,
        0x0B,
        0x0B,

        0x03,
        0x03,
        0x03,

        0x01,
        0x01,
        0x01,

        0x09,
        0x09
    };


    for (UINTN i = 0;
         i < 11;
         i++) {

        out->SetAttribute(
            out,
            logo_colors[i]
        );

        print(
            out,
            logo[i]
        );

        print(
            out,
            "\n"
        );
    }


    out->SetAttribute(
        out,
        0x0F
    );

    print(
        out,
        "\n"
    );


    /* --------------------------------------------------------
     * Header
     * -------------------------------------------------------- */

    out->SetAttribute(
        out,
        0x0B
    );

    print(
        out,
        "MyOS"
    );

    out->SetAttribute(
        out,
        0x08
    );

    print(
        out,
        " @ "
    );

    out->SetAttribute(
        out,
        0x0F
    );

    print(
        out,
        "UEFI bare-metal environment\n"
    );


    print_separator(out);


    /* --------------------------------------------------------
     * Operating system
     * -------------------------------------------------------- */

    print_label(
        out,
        "OS"
    );

    print(
        out,
        "        : MyOS 0.1"
    );

    print(
        out,
        " (x86_64, bare-metal UEFI)\n"
    );


    print_label(
        out,
        "Kernel"
    );

    print(
        out,
        "    : efi_main"
    );

    print(
        out,
        " (no Linux/Windows underneath)\n"
    );


    print_label(
        out,
        "Shell"
    );

    print(
        out,
        "     : myos-shell"
    );

    print(
        out,
        " (built-in command loop)\n"
    );


    print_label(
        out,
        "Architecture"
    );

    print(
        out,
        " : x86_64\n"
    );


    print_label(
        out,
        "Boot Mode"
    );

    print(
        out,
        "    : UEFI\n"
    );


    /* --------------------------------------------------------
     * Firmware
     * -------------------------------------------------------- */

    print_separator(out);


    print_label(
        out,
        "Firmware"
    );

    print(
        out,
        "  : "
    );


    if (st->FirmwareVendor)
        print16(
            out,
            st->FirmwareVendor
        );
    else
        print(
            out,
            "Unknown"
        );


    print(
        out,
        " rev "
    );


    print_uint(
        out,
        st->FirmwareRevision
    );


    print(
        out,
        "\n"
    );


    print_label(
        out,
        "UEFI"
    );

    print(
        out,
        "       : "
    );


    print_uint(
        out,
        (st->Hdr.Revision >> 16) &
        0xFFFF
    );


    print(
        out,
        "."
    );


    print_uint(
        out,
        st->Hdr.Revision &
        0xFFFF
    );


    print(
        out,
        "\n"
    );


    /* --------------------------------------------------------
     * Secure Boot
     * -------------------------------------------------------- */

    UINT8 secure_boot = 0;

    UINTN secure_boot_size =
        sizeof(secure_boot);


    EFI_GUID global_variable =
    {
        0x8BE4DF61,
        0x93CA,
        0x11D2,
        {
            0xAA,
            0x0D,
            0x00,
            0xE0,
            0x98,
            0x03,
            0x2B,
            0x8C
        }
    };


    EFI_STATUS sb_status =
        st->RuntimeServices->GetVariable(
            L"SecureBoot",
            &global_variable,
            NULL,
            &secure_boot_size,
            &secure_boot
        );


    print_label(
        out,
        "Secure Boot"
    );

    print(
        out,
        " : "
    );


    if (sb_status == EFI_SUCCESS) {

        print_bool(
            out,
            secure_boot != 0
        );

    } else {

        out->SetAttribute(
            out,
            0x08
        );

        print(
            out,
            "Unknown"
        );

        out->SetAttribute(
            out,
            0x0F
        );
    }


    print(
        out,
        "\n"
    );


    /* --------------------------------------------------------
     * Boot time / uptime
     * -------------------------------------------------------- */

    if (g_have_boot_time) {

        EFI_TIME now;


        if (st->RuntimeServices->GetTime &&
            st->RuntimeServices->GetTime(
                &now,
                NULL
            ) == EFI_SUCCESS) {


            INT64 now_secs =
                (INT64)now.Hour * 3600 +
                (INT64)now.Minute * 60 +
                now.Second;


            INT64 boot_secs =
                (INT64)g_boot_time.Hour * 3600 +
                (INT64)g_boot_time.Minute * 60 +
                g_boot_time.Second;


            INT64 secs =
                now_secs - boot_secs;


            if (secs < 0)
                secs += 86400;


            print_label(
                out,
                "Uptime"
            );

            print(
                out,
                "      : "
            );


            print_uint(
                out,
                (UINT64)secs / 3600
            );

            print(
                out,
                "h "
            );


            print_uint(
                out,
                ((UINT64)secs / 60) % 60
            );

            print(
                out,
                "m "
            );


            print_uint(
                out,
                (UINT64)secs % 60
            );

            print(
                out,
                "s\n"
            );
        }
    }


    /* --------------------------------------------------------
     * Current date/time
     * -------------------------------------------------------- */

    {
        EFI_TIME now;


        if (st->RuntimeServices->GetTime &&
            st->RuntimeServices->GetTime(
                &now,
                NULL
            ) == EFI_SUCCESS) {


            print_label(
                out,
                "Date"
            );

            print(
                out,
                "        : "
            );


            print_uint(
                out,
                now.Day
            );

            print(
                out,
                "."
            );


            print_uint(
                out,
                now.Month
            );

            print(
                out,
                "."
            );


            print_uint(
                out,
                now.Year
            );


            print(
                out,
                " "
            );


            if (now.Hour < 10)
                print(
                    out,
                    "0"
                );


            print_uint(
                out,
                now.Hour
            );


            print(
                out,
                ":"
            );


            if (now.Minute < 10)
                print(
                    out,
                    "0"
                );


            print_uint(
                out,
                now.Minute
            );


            print(
                out,
                ":"
            );


            if (now.Second < 10)
                print(
                    out,
                    "0"
                );


            print_uint(
                out,
                now.Second
            );


            print(
                out,
                "\n"
            );
        }
    }


    /* --------------------------------------------------------
     * GOP
     * -------------------------------------------------------- */

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop =
        NULL;


    EFI_GUID gop_guid =
        EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;


    EFI_STATUS gop_status =
        st->BootServices->LocateProtocol(
            &gop_guid,
            NULL,
            (void **)&gop
        );


    if (gop_status == EFI_SUCCESS &&
        gop != NULL) {


        print_separator(out);


        print_label(
            out,
            "GPU"
        );

        print(
            out,
            "        : EFI Graphics Output Protocol\n"
        );


        print_label(
            out,
            "Resolution"
        );

        print(
            out,
            " : "
        );


        if (gop->Mode &&
            gop->Mode->Info) {

            print_uint(
                out,
                gop->Mode->Info->
                    HorizontalResolution
            );


            print(
                out,
                "x"
            );


            print_uint(
                out,
                gop->Mode->Info->
                    VerticalResolution
            );


            print(
                out,
                "\n"
            );
        }


        print_label(
            out,
            "Pixel Format"
        );

        print(
            out,
            " : "
        );


        if (gop->Mode &&
            gop->Mode->Info) {


            switch (
                gop->Mode->Info->PixelFormat
            ) {

                case PixelRedGreenBlueReserved8BitPerColor:

                    print(
                        out,
                        "RGB"
                    );

                    break;


                case PixelBlueGreenRedReserved8BitPerColor:

                    print(
                        out,
                        "BGR"
                    );

                    break;


                case PixelBitMask:

                    print(
                        out,
                        "BitMask"
                    );

                    break;


                case PixelBltOnly:

                    print(
                        out,
                        "BltOnly"
                    );

                    break;


                default:

                    print(
                        out,
                        "Unknown"
                    );

                    break;
            }


            print(
                out,
                "\n"
            );
        }
    }


    /* --------------------------------------------------------
     * Console
     * -------------------------------------------------------- */

    print_separator(out);


    print_label(
        out,
        "Terminal"
    );

    print(
        out,
        "    : EFI_SIMPLE_TEXT_OUTPUT\n"
    );


    print_label(
        out,
        "Input"
    );

    print(
        out,
        "       : EFI_SIMPLE_TEXT_INPUT\n"
    );


    print_label(
        out,
        "Text Mode"
    );

    print(
        out,
        "   : "
    );


    if (out->Mode) {

        print_uint(
            out,
            out->Mode->Mode
        );


        print(
            out,
            " / "
        );


        if (out->Mode->MaxMode > 0)
            print_uint(
                out,
                out->Mode->MaxMode - 1
            );
        else
            print_uint(
                out,
                0
            );
    }


    print(
        out,
        "\n"
    );


    /* --------------------------------------------------------
     * UEFI services
     * -------------------------------------------------------- */

    print_label(
        out,
        "Boot Services"
    );

    print(
        out,
        " : "
    );


    print_bool(
        out,
        st->BootServices != NULL
    );


    print(
        out,
        "\n"
    );


    print_label(
        out,
        "Runtime Services"
    );

    print(
        out,
        " : "
    );


    print_bool(
        out,
        st->RuntimeServices != NULL
    );


    print(
        out,
        "\n"
    );


    /* --------------------------------------------------------
     * System table
     * -------------------------------------------------------- */

    print_label(
        out,
        "System Table"
    );

    print(
        out,
        "  : "
    );


    print_uint(
        out,
        (UINT64)(UINTN)st
    );


    print(
        out,
        "\n"
    );


    /* --------------------------------------------------------
     * Memory
     * -------------------------------------------------------- */

    print_separator(out);


    print_label(
        out,
        "Memory"
    );

    print(
        out,
        "      : UEFI memory map available\n"
    );


    print_label(
        out,
        "Allocator"
    );

    print(
        out,
        "   : EFI Boot Services\n"
    );


    /* --------------------------------------------------------
     * Palette
     *
     * Используем ### вместо "███".
     *
     * Причина:
     * print() работает с char* и не является UTF-8
     * декодером. Символ █ в UTF-8 занимает несколько
     * байтов и раньше мог превращаться в мусор.
     * -------------------------------------------------------- */

    print(
        out,
        "\n"
    );


    print_label(
        out,
        "Colors"
    );

    print(
        out,
        "      : "
    );


    for (UINTN i = 0;
         i < 8;
         i++) {

        out->SetAttribute(
            out,
            i
        );

        print(
            out,
            "###"
        );
    }


    out->SetAttribute(
        out,
        0x0F
    );


    print(
        out,
        "\n"
    );


    print_label(
        out,
        "Bright"
    );

    print(
        out,
        "      : "
    );


    for (UINTN i = 8;
         i < 16;
         i++) {

        out->SetAttribute(
            out,
            i
        );

        print(
            out,
            "###"
        );
    }


    out->SetAttribute(
        out,
        saved
    );


    print(
        out,
        "\n"
    );


    print_separator(out);


    /* --------------------------------------------------------
     * Footer
     * -------------------------------------------------------- */

    out->SetAttribute(
        out,
        0x08
    );


    print(
        out,
        "MyOS 0.1 | x86_64 | UEFI | bare-metal"
    );


    out->SetAttribute(
        out,
        0x0F
    );


    print(
        out,
        "\n\n"
    );
}


/* ============================================================
 * Command history
 * ============================================================ */

static void push_history(
    CHAR16 *line
)
{
    if (char16_len(line) == 0)
        return;


    char16_copy(
        g_history[
            g_history_count % HIST_MAX
        ],
        line,
        LINE_MAX
    );


    g_history_count++;
}


static void cmd_history(
    EFI_SYSTEM_TABLE *st
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;


    if (g_history_count == 0) {

        print(
            out,
            "(no history yet)\n"
        );

        return;
    }


    UINTN shown =
        (g_history_count < HIST_MAX)
        ? g_history_count
        : HIST_MAX;


    UINTN start =
        (g_history_count < HIST_MAX)
        ? 0
        : (g_history_count % HIST_MAX);


    for (UINTN i = 0;
         i < shown;
         i++) {


        UINTN idx =
            (start + i) % HIST_MAX;


        print_uint(
            out,
            g_history_count -
            shown +
            i +
            1
        );


        print(
            out,
            "  "
        );


        print16(
            out,
            g_history[idx]
        );


        print(
            out,
            "\n"
        );
    }
}


/* ============================================================
 * Command dispatcher
 * ============================================================ */

static void run_command(
    EFI_SYSTEM_TABLE *st,
    CHAR16 *line
)
{
    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        st->ConOut;


    push_history(line);


    /* --------------------------------------------------------
     * Empty
     * -------------------------------------------------------- */

    if (streq(line, "")) {

        return;


    /* --------------------------------------------------------
     * help
     * -------------------------------------------------------- */

    } else if (streq(line, "help")) {

        print(
            out,
            "Available commands:\n"
        );

        print(
            out,
            "  help          - show this list\n"
        );

        print(
            out,
            "  fetch         - show system info (like neofetch/fastfetch)\n"
        );

        print(
            out,
            "  about         - short info about MyOS\n"
        );

        print(
            out,
            "  ver           - print MyOS version\n"
        );

        print(
            out,
            "  banner        - reprint the startup banner\n"
        );

        print(
            out,
            "  time          - show current UEFI time\n"
        );

        print(
            out,
            "  date          - show current UEFI date\n"
        );

        print(
            out,
            "  uptime        - time elapsed since boot\n"
        );

        print(
            out,
            "  echo <text>   - print text back\n"
        );

        print(
            out,
            "  calc <a> <op> <b> - basic calculator (+ - * /)\n"
        );

        print(
            out,
            "  color <0-15>  - change text color (0=black..15=white)\n"
        );

        print(
            out,
            "  clear         - clear the screen\n"
        );

        print(
            out,
            "  history       - show recently run commands\n"
        );

        print(
            out,
            "  whoami        - print current user\n"
        );

        print(
            out,
            "  sleep <sec>   - pause for N seconds\n"
        );

        print(
            out,
            "  --- files (RAM disk, cleared on reboot) ---\n"
        );

        print(
            out,
            "  ls            - list files\n"
        );

        print(
            out,
            "  touch <name>  - create an empty file\n"
        );

        print(
            out,
            "  cat <name>    - print file contents\n"
        );

        print(
            out,
            "  write <n> <t> - overwrite file with text\n"
        );

        print(
            out,
            "  append <n> <t>- append text to file\n"
        );

        print(
            out,
            "  edit <name>   - multi-line editor, end with '.'\n"
        );

        print(
            out,
            "  rm <name>     - delete a file\n"
        );

        print(
            out,
            "  mv <a> <b>    - rename a file\n"
        );

        print(
            out,
            "  cp <a> <b>    - copy a file\n"
        );

        print(
            out,
            "  size <name>   - show file size in bytes\n"
        );

        print(
            out,
            "  reboot        - cold reboot\n"
        );

        print(
            out,
            "  shutdown      - power off the machine\n"
        );

        print(
            out,
            "  exit          - same as shutdown\n"
        );


    /* --------------------------------------------------------
     * whoami
     * -------------------------------------------------------- */

    } else if (streq(line, "whoami")) {

        print(
            out,
            "root@myos\n"
        );


    /* --------------------------------------------------------
     * history
     * -------------------------------------------------------- */

    } else if (streq(line, "history")) {

        cmd_history(st);


    /* --------------------------------------------------------
     * sleep
     * -------------------------------------------------------- */

    } else if (starts_with(line, "sleep ")) {

        UINTN secs =
            parse_uint(
                line + 6
            );


        for (UINTN i = 0;
             i < secs;
             i++) {

            st->BootServices->Stall(
                1000000
            );
        }


        print(
            out,
            "Woke up.\n"
        );


    /* --------------------------------------------------------
     * calc
     * -------------------------------------------------------- */

    } else if (starts_with(line, "calc ")) {

        CHAR16 tmp[LINE_MAX];


        char16_copy(
            tmp,
            line + 5,
            LINE_MAX
        );


        cmd_calc(
            st,
            tmp
        );


    /* --------------------------------------------------------
     * ls
     * -------------------------------------------------------- */

    } else if (
        streq(line, "ls") ||
        streq(line, "dir") ||
        streq(line, "files")
    ) {

        cmd_ls(st);


    /* --------------------------------------------------------
     * touch
     * -------------------------------------------------------- */

    } else if (starts_with(line, "touch ")) {

        CHAR16 name[FS_NAME_MAX];


        take_word(
            skip_ws16(line + 6),
            name,
            FS_NAME_MAX
        );


        cmd_touch(
            st,
            name
        );


    /* --------------------------------------------------------
     * cat
     * -------------------------------------------------------- */

    } else if (starts_with(line, "cat ")) {

        CHAR16 name[FS_NAME_MAX];


        take_word(
            skip_ws16(line + 4),
            name,
            FS_NAME_MAX
        );


        cmd_cat(
            st,
            name
        );


    /* --------------------------------------------------------
     * write
     * -------------------------------------------------------- */

    } else if (starts_with(line, "write ")) {

        CHAR16 name[FS_NAME_MAX];


        CHAR16 *rest =
            take_word(
                skip_ws16(line + 6),
                name,
                FS_NAME_MAX
            );


        rest =
            skip_ws16(rest);


        cmd_write(
            st,
            name,
            rest
        );


    /* --------------------------------------------------------
     * append
     * -------------------------------------------------------- */

    } else if (starts_with(line, "append ")) {

        CHAR16 name[FS_NAME_MAX];


        CHAR16 *rest =
            take_word(
                skip_ws16(line + 7),
                name,
                FS_NAME_MAX
            );


        rest =
            skip_ws16(rest);


        cmd_append(
            st,
            name,
            rest
        );


    /* --------------------------------------------------------
     * edit
     * -------------------------------------------------------- */

    } else if (starts_with(line, "edit ")) {

        CHAR16 name[FS_NAME_MAX];


        take_word(
            skip_ws16(line + 5),
            name,
            FS_NAME_MAX
        );


        cmd_edit(
            st,
            name
        );


    /* --------------------------------------------------------
     * rm
     * -------------------------------------------------------- */

    } else if (starts_with(line, "rm ")) {

        CHAR16 name[FS_NAME_MAX];


        take_word(
            skip_ws16(line + 3),
            name,
            FS_NAME_MAX
        );


        cmd_rm(
            st,
            name
        );


    /* --------------------------------------------------------
     * mv
     * -------------------------------------------------------- */

    } else if (starts_with(line, "mv ")) {

        CHAR16 a[FS_NAME_MAX];
        CHAR16 b[FS_NAME_MAX];


        CHAR16 *rest =
            take_word(
                skip_ws16(line + 3),
                a,
                FS_NAME_MAX
            );


        rest =
            skip_ws16(rest);


        take_word(
            rest,
            b,
            FS_NAME_MAX
        );


        cmd_mv(
            st,
            a,
            b
        );


    /* --------------------------------------------------------
     * cp
     * -------------------------------------------------------- */

    } else if (starts_with(line, "cp ")) {

        CHAR16 a[FS_NAME_MAX];
        CHAR16 b[FS_NAME_MAX];


        CHAR16 *rest =
            take_word(
                skip_ws16(line + 3),
                a,
                FS_NAME_MAX
            );


        rest =
            skip_ws16(rest);


        take_word(
            rest,
            b,
            FS_NAME_MAX
        );


        cmd_cp(
            st,
            a,
            b
        );


    /* --------------------------------------------------------
     * size
     * -------------------------------------------------------- */

    } else if (starts_with(line, "size ")) {

        CHAR16 name[FS_NAME_MAX];


        take_word(
            skip_ws16(line + 5),
            name,
            FS_NAME_MAX
        );


        int idx =
            fs_find(name);


        if (idx < 0) {

            print(
                out,
                "No such file.\n"
            );

        } else {

            print_uint(
                out,
                g_fs[idx].size
            );

            print(
                out,
                " bytes\n"
            );
        }


    /* --------------------------------------------------------
     * fetch
     * -------------------------------------------------------- */

    } else if (
        streq(line, "fetch") ||
        streq(line, "neofetch") ||
        streq(line, "fastfetch")
    ) {

        cmd_fetch(st);


    /* --------------------------------------------------------
     * about
     * -------------------------------------------------------- */

    } else if (streq(line, "about")) {

        print(
            out,
            "MyOS 0.1 - a minimal 64-bit UEFI OS built from scratch.\n"
        );

        print(
            out,
            "No Linux, no Windows, no GNU-EFI/EDK2 - just gcc + ld.\n"
        );

        print(
            out,
            "Type 'fetch' for a system summary or 'help' for commands.\n"
        );


    /* --------------------------------------------------------
     * version
     * -------------------------------------------------------- */

    } else if (
        streq(line, "ver") ||
        streq(line, "version")
    ) {

        print(
            out,
            "MyOS 0.1\n"
        );


    /* --------------------------------------------------------
     * banner
     * -------------------------------------------------------- */

    } else if (streq(line, "banner")) {

        print(
            out,
            "================================\n"
        );

        print(
            out,
            "   MyOS 0.1 - kernel base\n"
        );

        print(
            out,
            "   64-bit UEFI, no Linux/Windows\n"
        );

        print(
            out,
            "================================\n"
        );


    /* --------------------------------------------------------
     * time
     * -------------------------------------------------------- */

    } else if (streq(line, "time")) {

        EFI_TIME now;


        if (
            st->RuntimeServices->GetTime &&
            st->RuntimeServices->GetTime(
                &now,
                NULL
            ) == EFI_SUCCESS
        ) {

            print_uint2(
                out,
                now.Hour
            );

            print(
                out,
                ":"
            );

            print_uint2(
                out,
                now.Minute
            );

            print(
                out,
                ":"
            );

            print_uint2(
                out,
                now.Second
            );

            print(
                out,
                "\n"
            );

        } else {

            print(
                out,
                "Time service unavailable.\n"
            );
        }


    /* --------------------------------------------------------
     * date
     * -------------------------------------------------------- */

    } else if (streq(line, "date")) {

        EFI_TIME now;


        if (
            st->RuntimeServices->GetTime &&
            st->RuntimeServices->GetTime(
                &now,
                NULL
            ) == EFI_SUCCESS
        ) {

            print_uint2(
                out,
                now.Day
            );

            print(
                out,
                "-"
            );

            print_uint2(
                out,
                now.Month
            );

            print(
                out,
                "-"
            );

            print_uint(
                out,
                now.Year
            );

            print(
                out,
                "\n"
            );

        } else {

            print(
                out,
                "Date service unavailable.\n"
            );
        }


    /* --------------------------------------------------------
     * uptime
     * -------------------------------------------------------- */

    } else if (streq(line, "uptime")) {

        if (
            !g_have_boot_time ||
            !st->RuntimeServices->GetTime
        ) {

            print(
                out,
                "Uptime unavailable.\n"
            );

        } else {

            EFI_TIME now;


            if (
                st->RuntimeServices->GetTime(
                    &now,
                    NULL
                ) == EFI_SUCCESS
            ) {

                INT64 secs =
                    (INT64)now.Hour * 3600 +
                    (INT64)now.Minute * 60 +
                    now.Second
                    -
                    (
                        (INT64)g_boot_time.Hour * 3600 +
                        (INT64)g_boot_time.Minute * 60 +
                        g_boot_time.Second
                    );


                if (secs < 0)
                    secs += 86400;


                print(
                    out,
                    "up "
                );


                print_uint(
                    out,
                    (UINT64)secs / 3600
                );


                print(
                    out,
                    "h "
                );


                print_uint(
                    out,
                    ((UINT64)secs / 60) % 60
                );


                print(
                    out,
                    "m "
                );


                print_uint(
                    out,
                    (UINT64)secs % 60
                );


                print(
                    out,
                    "s\n"
                );
            }
        }


    /* --------------------------------------------------------
     * echo
     * -------------------------------------------------------- */

    } else if (starts_with(line, "echo ")) {

        /*
         * print16(), чтобы echo попадал
         * в scrollback.
         */
        print16(
            out,
            line + 5
        );

        print(
            out,
            "\n"
        );


    } else if (streq(line, "echo")) {

        print(
            out,
            "\n"
        );


    /* --------------------------------------------------------
     * color
     * -------------------------------------------------------- */

    } else if (starts_with(line, "color ")) {

        UINTN c =
            parse_uint(
                line + 6
            );


        if (c > 15) {

            print(
                out,
                "Usage: color <0-15>\n"
            );

        } else {

            g_color = c;


            out->SetAttribute(
                out,
                g_color
            );


            print(
                out,
                "Color changed.\n"
            );
        }


    /* --------------------------------------------------------
     * clear
     * -------------------------------------------------------- */

    } else if (
        streq(line, "clear") ||
        streq(line, "cls")
    ) {

        out->ClearScreen(out);


    /* --------------------------------------------------------
     * reboot
     * -------------------------------------------------------- */

    } else if (streq(line, "reboot")) {

        print(
            out,
            "Rebooting...\n"
        );


        st->RuntimeServices->ResetSystem(
            EfiResetCold,
            EFI_SUCCESS,
            0,
            NULL
        );


    /* --------------------------------------------------------
     * shutdown
     * -------------------------------------------------------- */

    } else if (
        streq(line, "shutdown") ||
        streq(line, "exit")
    ) {

        print(
            out,
            "Shutting down...\n"
        );


        st->RuntimeServices->ResetSystem(
            EfiResetShutdown,
            EFI_SUCCESS,
            0,
            NULL
        );


    /* --------------------------------------------------------
     * unknown command
     * -------------------------------------------------------- */

    } else {

        print(
            out,
            "Unknown command: "
        );


        /*
         * Используем print16(), чтобы этот вывод
         * тоже попал в scrollback.
         */
        print16(
            out,
            line
        );


        print(
            out,
            "\n(type 'help')\n"
        );
    }
}


/* ============================================================
 * UEFI entry point
 * ============================================================ */

EFI_STATUS EFIAPI efi_main(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
)
{
    (void)ImageHandle;


    SIMPLE_TEXT_OUTPUT_INTERFACE *out =
        SystemTable->ConOut;


    out->Reset(
        out,
        FALSE
    );


    out->ClearScreen(out);


    out->SetAttribute(
        out,
        g_color
    );


    /* --------------------------------------------------------
     * Save boot time
     * -------------------------------------------------------- */

    if (
        SystemTable->RuntimeServices->GetTime &&
        SystemTable->RuntimeServices->GetTime(
            &g_boot_time,
            NULL
        ) == EFI_SUCCESS
    ) {

        g_have_boot_time = TRUE;
    }


    /* --------------------------------------------------------
     * Startup banner
     * -------------------------------------------------------- */

    print(
        out,
        "================================\n"
    );

    print(
        out,
        "   MyOS 0.1 - kernel base\n"
    );

    print(
        out,
        "   64-bit UEFI, no Linux/Windows\n"
    );

    print(
        out,
        "================================\n\n"
    );


    print(
        out,
        "Boot services: OK\n"
    );

    print(
        out,
        "Text output:   OK\n\n"
    );


    print(
        out,
        "Type 'help' for the list of commands, or 'fetch' for a system summary.\n\n"
    );


    /* --------------------------------------------------------
     * Main shell loop
     * -------------------------------------------------------- */

    CHAR16 line[LINE_MAX];


    for (;;) {

        out->SetAttribute(
            out,
            g_color
        );


        print(
            out,
            "> "
        );


        read_line(
            SystemTable,
            line,
            LINE_MAX
        );


        run_command(
            SystemTable,
            line
        );
    }


    return EFI_SUCCESS;
}
