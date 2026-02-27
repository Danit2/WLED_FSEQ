Import("env")

projenv = env

# Read custom_usermods from platformio.ini
custom_usermods = projenv.GetProjectOption("custom_usermods", default="")

# Normalize list (comma or space separated)
usermod_list = [
    u.strip().upper()
    for u in custom_usermods.replace(",", " ").split()
    if u.strip()
]

# Flags
fseq_enabled = "FSEQ" in usermod_list or "*" in usermod_list
sdcard_present = "SD_CARD" in usermod_list or "*" in usermod_list

# ------------------------------------------------------------------
# Auto-add SD_CARD if FSEQ is enabled
# ------------------------------------------------------------------
if fseq_enabled and not sdcard_present:
    print("FSEQ detected -> auto-adding SD_CARD usermod")

    # Append SD_CARD to list
    usermod_list.append("SD_CARD")

    # Rebuild string (space separated)
    new_value = " ".join(usermod_list)

    # Override project option
    projenv.Replace(custom_usermods=new_value)

# ------------------------------------------------------------------
# Ensure SD driver is enabled (SPI default fallback)
# ------------------------------------------------------------------

cpp_defines = projenv.get("CPPDEFINES", [])

define_names = []
for d in cpp_defines:
    if isinstance(d, tuple):
        define_names.append(d[0])
    else:
        define_names.append(d)

mmc_enabled = "WLED_USE_SD_MMC" in define_names
spi_enabled = "WLED_USE_SD_SPI" in define_names

if fseq_enabled and not mmc_enabled and not spi_enabled:
    print("FSEQ detected -> enabling WLED_USE_SD_SPI")
    projenv.Append(CPPDEFINES=["WLED_USE_SD_SPI"])