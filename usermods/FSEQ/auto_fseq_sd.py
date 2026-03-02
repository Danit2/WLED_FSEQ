Import("env")

projenv = env

# -------------------------------------------------
# Read custom_usermods exactly as defined
# -------------------------------------------------
custom_usermods = projenv.GetProjectOption("custom_usermods", default="")

# Split but DO NOT change case
usermod_list = [
    u.strip()
    for u in custom_usermods.replace(",", " ").split()
    if u.strip()
]

print("Original custom_usermods:", usermod_list)

# -------------------------------------------------
# Detect flags (case sensitive!)
# -------------------------------------------------
fseq_enabled = "FSEQ" in usermod_list or "*" in usermod_list
sdcard_present = "sd_card" in usermod_list or "*" in usermod_list

# -------------------------------------------------
# Auto-add sd_card if FSEQ is enabled
# -------------------------------------------------
if fseq_enabled and not sdcard_present:
    print("FSEQ detected -> auto-adding sd_card usermod")

    usermod_list.append("sd_card")

    # rebuild string (space separated)
    new_value = " ".join(usermod_list)

    print("New custom_usermods:", new_value)

    # Override PROJECT_OPTIONS (only works with pre:)
    env["PROJECT_OPTIONS"]["custom_usermods"] = new_value

# -------------------------------------------------
# Ensure SD driver is enabled (SPI fallback)
# -------------------------------------------------
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