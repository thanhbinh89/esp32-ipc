# Layered layout:
#   hal/  thin wrappers over IDF drivers and components, no app policy
#   core/ app services shared by tasks, no FreeRTOS task ownership
#   task/ one FreeRTOS task per file
set(priv_requires esp_netif esp_video
                  esp_timer esp_driver_ppa esp_driver_i2s esp_driver_i2c
                  esp_mm esp-dl pedestrian_detect esp_codec_dev libpeer)

# hal_netif.c already includes esp_eth.h / esp_wifi.h under the matching #if, so
# the requirement follows it. Listing both unconditionally made MINIMAL_BUILD
# compile the whole esp_eth driver into every Wi-Fi build (12 objects the linker
# then discards whole, since nothing references them).
#
# This cannot test CONFIG_APP_NETIF_ETH. IDF expands component requirements in a
# separate "cmake -P" child process (component.cmake, __component_get_requirements)
# which is handed only the build- and component-property files -- no
# sdkconfig.cmake -- so every CONFIG_* is empty there and the else() branch always
# won. That is what put esp_wifi into an Ethernet build and left esp_eth.h
# unfound, while the main pass, where CONFIG_* does exist, still listed esp_eth in
# PRIV_REQUIRES: the two passes disagreed and the requirements pass is the one
# that decides what gets built.
#
# Reading the file works in both passes. If it cannot be read -- a fresh tree,
# before the first Kconfig run -- ask for both: a slightly fatter build beats a
# broken one, and the next configure narrows it again.
set(_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig")
if(EXISTS "${_sdkconfig}")
    file(STRINGS "${_sdkconfig}" _netif_eth  REGEX "^CONFIG_APP_NETIF_ETH=y$")
    file(STRINGS "${_sdkconfig}" _netif_wifi REGEX "^CONFIG_APP_NETIF_WIFI=y$")
else()
    set(_netif_eth TRUE)
    set(_netif_wifi TRUE)
endif()
if(_netif_eth)
    list(APPEND priv_requires esp_eth)
endif()
if(_netif_wifi)
    list(APPEND priv_requires esp_wifi)
endif()

idf_component_register(SRC_DIRS "." "hal" "core" "task"
                       INCLUDE_DIRS "." "hal" "core" "task"
                       PRIV_REQUIRES ${priv_requires})

# libpeer keeps its tunables in its own src/config.h, every one behind #ifndef.
# Force-including core/libpeer_config.h ahead of it makes our definitions win, so
# the values the app and the RTP layer must agree on (frame rate, audio packet
# duration) have exactly one definition. Patching config.h would work too, but
# managed_components/ is regenerated from the registry.
idf_component_get_property(libpeer_lib sepfy__libpeer COMPONENT_LIB)
target_compile_options(${libpeer_lib} PRIVATE
                       "-include${CMAKE_CURRENT_SOURCE_DIR}/core/libpeer_config.h")
