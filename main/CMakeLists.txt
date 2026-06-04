idf_component_register(SRCS "video_task.cpp" "pedestrian_detect_task.cpp" "osd.c"
                            "app_camera_pipeline.cpp" "app_video.c"
                            "app_audio_codec.c" "speaker_task.cpp" "audio_task.cpp" 
                            "webrtc_task.cpp" "app_ethernet.c" "ipc.cpp"
                       PRIV_REQUIRES esp_netif esp_eth esp_video
                                     esp_timer esp_driver_ppa esp_driver_i2s esp_driver_i2c
                                     esp_mm esp-dl pedestrian_detect esp_codec_dev libpeer
                       INCLUDE_DIRS "."
                       EMBED_FILES "canon.pcm")
