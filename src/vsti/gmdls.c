/* gmdls.c -- embeds dist/gm.dls via .incbin into the DLL. i686 PE
 * prefixes asm symbol names with "_", hence __USER_LABEL_PREFIX__. */
#define GMDLS_STR(x) #x
#define GMDLS_XSTR(x) GMDLS_STR(x)
#define GMDLS_PREFIX GMDLS_XSTR(__USER_LABEL_PREFIX__)
#define GMDLS_BLOB_SYM GMDLS_PREFIX "gm_dls_blob"
#define GMDLS_END_SYM GMDLS_PREFIX "gm_dls_blob_end"
#define GMDLS_LEN_SYM GMDLS_PREFIX "gm_dls_blob_len"

/* PE .rodata lacks the READONLY section flag; "dr" gives a real
 * read-only section there. ELF .rodata is already read-only. */
#if defined(_WIN32)
#define GMDLS_SECTION ".section .rdata,\"dr\"\n"
#else
#define GMDLS_SECTION ".section .rodata\n"
#endif

__asm__(GMDLS_SECTION ".balign 8\n"
                      ".global " GMDLS_BLOB_SYM "\n" GMDLS_BLOB_SYM ":\n"
                      ".incbin \"dist/gm.dls\"\n" GMDLS_END_SYM ":\n"
                      ".balign 4\n"
                      ".global " GMDLS_LEN_SYM "\n" GMDLS_LEN_SYM ":\n"
                      ".long " GMDLS_END_SYM " - " GMDLS_BLOB_SYM "\n");

extern const unsigned char gm_dls_blob[];
extern const unsigned int gm_dls_blob_len;
