/* Force-included in every translation unit of the native Linux build (scripts/build.sh).
 *
 * The build machine's glibc (Arch, 2.44) gave some libm functions new symbol versions
 * (GLIBC_2.43 / 2.44), and binaries linked against them do not load on an older glibc such
 * as the Steam Deck's (2.41). The functions themselves exist there under the x86-64 baseline
 * version GLIBC_2.2.5: bind them to it. Add a line here when the install stage reports another
 * symbol version newer than the Deck's (`objdump -T ... | grep GLIBC_2.4`). */
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver asinf,asinf@GLIBC_2.2.5");
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver cosh,cosh@GLIBC_2.2.5");
__asm__(".symver sinh,sinh@GLIBC_2.2.5");
#endif
