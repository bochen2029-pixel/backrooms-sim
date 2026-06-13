// stb_impl.cpp — the single translation unit that instantiates the stb image
// implementations. Isolated in the br_stb target (warnings disabled) so this
// third-party code never trips the project's /WX.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include <stb_image.h>
#include <stb_image_write.h>
