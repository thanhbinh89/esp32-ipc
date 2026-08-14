idf_component_register(SRCS "app_audio.c" "video_task.cpp" "pedestrian_detect_task.cpp" "osd.c"
                            "app_camera_pipeline.cpp" "app_video.c"
                            "task_speaker.cpp" "task_audio.cpp" 
                            "task_webrtc.cpp" "app_ethernet.c" "app_main.cpp"
                       PRIV_REQUIRES esp_netif esp_eth esp_wifi esp_video
                                     esp_timer esp_driver_ppa esp_driver_i2s esp_driver_i2c
                                     esp_mm esp-dl pedestrian_detect esp_codec_dev libpeer
                       INCLUDE_DIRS "."
                       EMBED_FILES "canon.pcm")
