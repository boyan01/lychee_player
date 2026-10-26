// The default fonts for libass where the system has no font provider
// (Android): LYCHEE_DEFAULT_FONT_REGULAR and LYCHEE_DEFAULT_FONT_BOLD,
// compiled in, so that every caller of the C ABI has them without files of
// their own. ELF only.

  .section .rodata
  .balign 16
  .global lychee_default_font_regular
  .hidden lychee_default_font_regular
lychee_default_font_regular:
  .incbin LYCHEE_DEFAULT_FONT_REGULAR
  .global lychee_default_font_regular_end
  .hidden lychee_default_font_regular_end
lychee_default_font_regular_end:

  .balign 16
  .global lychee_default_font_bold
  .hidden lychee_default_font_bold
lychee_default_font_bold:
  .incbin LYCHEE_DEFAULT_FONT_BOLD
  .global lychee_default_font_bold_end
  .hidden lychee_default_font_bold_end
lychee_default_font_bold_end:

  .section .note.GNU-stack, "", %progbits
