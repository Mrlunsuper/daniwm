/* Internal regression checks: compile in the real implementation. */
#define main dani_run_main
#include "../src/run.c"
#undef main
#include <assert.h>

int main(void) {
    KeySym ks = XK_Return;
    assert(lookup_length(100, 64, 1, XBufferOverflow, &ks) == -1);
    assert(lookup_length(64, 64, 0, XLookupNone, &ks) == -1);
    assert(lookup_length(-1, 64, 1, XLookupNone, &ks) == -1);
    assert(lookup_length(63, 64, 1, XLookupBoth, &ks) == 63);
    assert(lookup_length(4, 64, 1, XLookupKeySym, &ks) == 0);
    assert(lookup_length(4, 64, 1, XLookupChars, &ks) == 4 && ks == NoSymbol);
    puts("PASS: IM overflow, boundary and lookup status guards");
    assert(valid_run_number("lines", "8"));
    assert(valid_run_number("cell_w", "36"));
    assert(!valid_run_number("lines", "17"));
    assert(!valid_run_number("cell_w", "36px"));
    assert(!valid_run_number("padding", "-1"));
    puts("PASS: compact grid config limits and strict numeric validation");
    double v;
    assert(calc_eval("-2^2", &v) && v == -4);
    assert(calc_eval("(-2)^2", &v) && v == 4);
    assert(calc_eval("2^-2", &v) && v == .25);
    assert(calc_eval("2^3^2", &v) && v == 512);
    assert(!calc_eval("exp(1000)", &v));
    assert(!calc_eval("1e999", &v));
    assert(!calc_eval("sqrt(-1)", &v));
    assert(!calc_eval("1/0", &v));
    puts("PASS: calculator precedence and finite results");

    char comment[] = "/bin/echo \"a#b\" 'c # d' e\\#f # comment";
    strip_comment(comment);
    assert(!strcmp(comment, "/bin/echo \"a#b\" 'c # d' e\\#f"));
    puts("PASS: quoted and escaped config comments");

    char *cmd = desktop_exec("/bin/echo \"$HOME\" \"*\" \"~\" %c %k %i %% %f \"\"",
                             "O'Brien App", "/tmp/test app.desktop", "test-icon");
    assert(cmd);
    wordexp_t w = {0};
    assert(!wordexp(cmd, &w, WRDE_NOCMD));
    const char *expected[] = {"/bin/echo", "$HOME", "*", "~", "O'Brien App",
                              "/tmp/test app.desktop", "--icon", "test-icon", "%", ""};
    assert(w.we_wordc == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < w.we_wordc; i++) assert(!strcmp(w.we_wordv[i], expected[i]));
    wordfree(&w); free(cmd);
    assert(!desktop_exec("/bin/echo %z", "name", "path", ""));
    assert(!desktop_exec("/bin/echo \"unterminated", "name", "path", ""));
    launch("echo | cat", "invalid"); /* Invalid wordexp must not free garbage. */
    puts("PASS: literal desktop argv and field expansion, invalid command cleanup");

    WW = 800; HH = 400; nfilt = 20; scroll = 0;
    mode = MODE_FAV; opt_cell_gap_x = 10; opt_cell_gap_y = 10;
    int gx = (WW - (opt_cols * cell_w + opt_cell_gap_x * (opt_cols - 1))) / 2;
    assert(hit_entry(gx + 10, input_h - 1) == -1);
    assert(hit_entry(gx - 1, input_h + 10) == -1);
    assert(hit_entry(gx + cell_w, input_h + 10) == -1);
    assert(hit_entry(gx + 10, input_h + cell_h) == -1);
    assert(hit_entry(gx + 10, input_h + 10) == 0);
    mode = MODE_DRUN;
    assert(hit_entry(100, input_h - 1) == -1);
    assert(hit_entry(100, input_h + row_h - 1) == -1); /* row gap */
    assert(hit_entry(0, input_h + 10) == -1);
    assert(hit_entry(100, input_h + 10) == 0);
    mode = MODE_CALC;
    assert(hit_entry(100, input_h + 10) == -1);
    puts("PASS: hit testing rejects input, margins, gaps and calculator");

    hist_save("audit-history");
    char path[1024]; hist_path(path, sizeof(path));
    assert(!access(path, F_OK));
    for (int i = 0; i < 12; i++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) { hist_save("audit-history"); _exit(0); }
    }
    for (int i = 0; i < 12; i++) { int st; assert(wait(&st) > 0 && WIFEXITED(st) && !WEXITSTATUS(st)); }
    FILE *f = fopen(path, "r"); assert(f);
    int count = 0; assert(fscanf(f, "%d", &count) == 1 && count == 13); fclose(f);
    f = fopen(path, "w"); assert(f);
    fprintf(f, "%d\taudit-history\n", INT_MAX); fclose(f);
    hist_save("audit-history");
    f = fopen(path, "r"); assert(f);
    assert(fscanf(f, "%d", &count) == 1 && count == INT_MAX); fclose(f);
    puts("PASS: fresh-cache history, concurrent updates and saturation");
    return 0;
}
