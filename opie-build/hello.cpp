#include <qapplication.h>
#include <qlabel.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    
    QLabel label("Hello from Qt/E 2.3.10!\nCasio BE-300 Emulator\nNEC VR4131 MIPS", 0);
    label.setAlignment(Qt::AlignCenter);
    
    app.setMainWidget(&label);
    label.show();
    
    return app.exec();
}
