#ifndef BE300_LAUNCHER_H
#define BE300_LAUNCHER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Launcher entry point. Called from main() when argv is empty (after filtering
 * macOS LaunchServices -psn_* arguments) or when argv[1] is a .be300vm bundle
 * path. Returns a process exit code. */
int launcher_main(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_H */
