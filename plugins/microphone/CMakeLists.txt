set(kdeconnect_microphone_SRCS
    microphoneplugin.cpp
    qtaudiooutput.cpp
)
if(WIN32)
    list(APPEND kdeconnect_microphone_SRCS wasapiaudiooutput.cpp)
endif()

kdeconnect_add_plugin(kdeconnect_microphone SOURCES ${kdeconnect_microphone_SRCS})
target_link_libraries(kdeconnect_microphone
    kdeconnectcore
    Qt::Multimedia
    KF6::I18n
)
if(WIN32)
    target_link_libraries(kdeconnect_microphone ole32 avrt)
endif()
