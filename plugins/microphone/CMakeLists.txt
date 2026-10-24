kdeconnect_add_plugin(kdeconnect_microphone SOURCES microphoneplugin.cpp)
target_link_libraries(kdeconnect_microphone
    kdeconnectcore
    Qt::Multimedia
    KF6::I18n
)
