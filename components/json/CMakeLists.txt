# Compatibility component for third-party ESP-IDF 5 projects that still
# declare `REQUIRES json`.  ESP-IDF 6 resolves cJSON from the Component
# Registry as `espressif__cjson`.
idf_component_register(REQUIRES espressif__cjson)
