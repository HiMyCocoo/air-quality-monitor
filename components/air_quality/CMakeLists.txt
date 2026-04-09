idf_component_register(SRCS "air_quality.c"
                         "air_quality_aqi.c"
                         "air_quality_labels.c"
                         "air_quality_particle.c"
                         "air_quality_rain.c"
                    INCLUDE_DIRS "include"
                    REQUIRES platform)
