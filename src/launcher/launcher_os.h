#ifndef BE300_LAUNCHER_OS_H
#define BE300_LAUNCHER_OS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reveal a path in the platform's file manager (Finder / Explorer / Files).
 * Best-effort — silently returns if the platform handler isn't available. */
void launcher_reveal_in_file_manager(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* BE300_LAUNCHER_OS_H */
