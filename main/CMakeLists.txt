# Layered layout:
#   hal/  thin wrappers over IDF drivers and components, no app policy
#   core/ app services shared by tasks, no FreeRTOS task ownership
#   task/ one FreeRTOS task per file
idf_component_register(SRC_DIRS "." "hal" "core" "task"
                       INCLUDE_DIRS "." "hal" "core" "task"
                       PRIV_REQUIRES esp_netif esp_eth esp_wifi esp_video
                                     esp_timer esp_driver_ppa esp_driver_i2s esp_driver_i2c
                                     esp_mm esp-dl pedestrian_detect esp_codec_dev libpeer)

# libpeer keeps its tunables in its own src/config.h, every one behind #ifndef.
# Force-including core/libpeer_config.h ahead of it makes our definitions win, so
# the values the app and the RTP layer must agree on (frame rate, audio packet
# duration) have exactly one definition. Patching config.h would work too, but
# managed_components/ is regenerated from the registry.
idf_component_get_property(libpeer_lib sepfy__libpeer COMPONENT_LIB)
target_compile_options(${libpeer_lib} PRIVATE
                       "-include${CMAKE_CURRENT_SOURCE_DIR}/core/libpeer_config.h")
