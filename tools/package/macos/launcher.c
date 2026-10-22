/* ghostldr.app's executable (Contents/MacOS/ghostldr).
 *
 * Runs Contents/Resources/ghostldr-app/app/launch.py --workspace with the
 * Python built for this Mac's architecture: Contents/Resources/python/arm64
 * or python/x86_64.  build_macos.py compiles it universal (arm64 + x86_64),
 * so each Mac runs its own slice and picks its own Python.
 *
 * Opened from the Finder, the app runs in the background: Python starts in a
 * session of its own and this process ends, so the next open (after the page
 * was closed, say) starts launch.py again, which finds the running app and
 * opens its page.  Its output goes to ~/Library/Logs/ghostldr/app.log.  Run
 * from a terminal, it stays in the foreground and prints there.
 */
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__arm64__) || defined(__aarch64__)
#define ARCH "arm64"
#else
#define ARCH "x86_64"
#endif

#define LOG_LIMIT (1 << 20)

static void alert(const char *text)
{
    char script[PATH_MAX + 256];
    fprintf(stderr, "ghostldr: %s\n", text);
    snprintf(script, sizeof script, "display alert \"ghostldr can't start\" message \"%s\"", text);
    execl("/usr/bin/osascript", "osascript", "-e", script, (char *)NULL);
}

static void log_to_file(void)
{
    const char *home = getenv("HOME");
    char dir[PATH_MAX], path[PATH_MAX];
    struct stat st;
    if (!home || !*home)
        return;
    snprintf(dir, sizeof dir, "%s/Library", home);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/Library/Logs", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Library/Logs/ghostldr", home);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/app.log", dir);
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (stat(path, &st) == 0 && st.st_size > LOG_LIMIT)
        flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
}

int main(int argc, char **argv)
{
    char exe[PATH_MAX], real[PATH_MAX], res[PATH_MAX], python[PATH_MAX], launch[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) {
        alert("Can't find the app's own folder.");
        return 1;
    }
    /* real: .../ghostldr.app/Contents/MacOS/ghostldr */
    for (int i = 0; i < 2; i++) {
        char *slash = strrchr(real, '/');
        if (!slash) {
            alert("Can't find the app's own folder.");
            return 1;
        }
        *slash = '\0';
    }
    snprintf(res, sizeof res, "%s/Resources", real);
    snprintf(python, sizeof python, "%s/python/" ARCH "/bin/python3", res);
    snprintf(launch, sizeof launch, "%s/ghostldr-app/app/launch.py", res);
    if (access(python, X_OK) != 0 || access(launch, R_OK) != 0) {
        alert("The app is incomplete: download it again and copy ghostldr to Applications.");
        return 1;
    }

    if (!isatty(STDERR_FILENO)) {
        log_to_file();
        pid_t pid = fork();
        if (pid > 0)
            return 0; /* LaunchServices sees the app finish; Python carries on */
        if (pid == 0)
            setsid();
        /* fork failed: run in the foreground */
    }
    /* Only this app's Python and files: nothing from the user's own Python
       setup, and nothing written inside the (signed) app. */
    unsetenv("PYTHONHOME");
    unsetenv("PYTHONPATH");
    unsetenv("PYTHONSTARTUP");
    unsetenv("PYTHONPYCACHEPREFIX");
    setenv("PYTHONNOUSERSITE", "1", 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);

    char **args = calloc((size_t)argc + 4, sizeof *args);
    if (!args)
        return 1;
    int n = 0;
    args[n++] = python;
    args[n++] = launch;
    args[n++] = "--workspace";
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "-psn_", 5) != 0) /* old macOS: the Finder's process serial number */
            args[n++] = argv[i];
    args[n] = NULL;
    execv(python, args);
    alert("The app's Python didn't start.");
    return 1;
}
