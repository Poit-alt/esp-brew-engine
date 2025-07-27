idf_component_register(SRCS "brew-engine.cpp" "max31865_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver nvs_flash esp_http_server esp_http_client onewire_bus mqtt settings-manager statistics-manager app_update
                    EMBED_FILES "index.html.gz" "manifest.json" "logo.svg.gz")