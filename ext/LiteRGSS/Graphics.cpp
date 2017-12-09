#include "ruby.h"
#include "ruby/encoding.h"
#include "ruby/thread.h"
#include "LiteRGSS.h"
#include "Graphics.h"

/* Variables definition */
VALUE rb_mGraphics = Qnil;
VALUE rb_eStoppedGraphics = Qnil;
VALUE rb_eClosedWindow = Qnil;
sf::RenderWindow* game_window = nullptr;
bool InsideGraphicsUpdate = false;
std::vector<CDrawable_Element*> Graphics_stack;
/* Structure definition */
struct GraphicUpdateMessage {
    VALUE errorObject;
    const char* message;
};
/* Functions definition */
void __LoadVideoModeFromConfigs(sf::VideoMode& vmode);
const sf::String __LoadTitleFromConfigs();
unsigned int __LoadFrameRateFromConfigs();
bool __LoadVSYNCFromConfigs();
void* __Graphics_Update_Internal(void* data);
VALUE __Graphics_Update_RaiseError(VALUE self, GraphicUpdateMessage* message);
void __Graphics_Update_Process_Event(GraphicUpdateMessage*& message);
void __Graphics_Update_Draw(std::vector<CDrawable_Element*>* stack);
/* Macro Definition */
#define GRAPHICS_PROTECT if(game_window == nullptr) { \
    rb_raise(rb_eStoppedGraphics, "Graphics is not started, window closed thus no Graphics operation allowed. Please call Graphics.start before using other Graphics functions."); \
    return self; \
}
void Init_Graphics()
{
    rb_mGraphics = rb_define_module_under(rb_mLiteRGSS, "Graphics");
    /* Defining the Stopped Graphics Error */
    rb_eStoppedGraphics = rb_define_class_under(rb_mGraphics, "StoppedError", rb_eStandardError);
    rb_eClosedWindow = rb_define_class_under(rb_mGraphics, "ClosedWindowError", rb_eStandardError);
    /* Defining the Graphics functions */
    rb_define_module_function(rb_mGraphics, "start", _rbf rb_Graphics_start, 0);
    rb_define_module_function(rb_mGraphics, "stop", _rbf rb_Graphics_stop, 0);
    rb_define_module_function(rb_mGraphics, "update", _rbf rb_Graphics_update, 0);
    /* creating the element table */
    rb_ivar_set(rb_mGraphics, rb_iElementTable, rb_ary_new());
}

VALUE rb_Graphics_start(VALUE self)
{
    if(game_window != nullptr)
        return Qnil;
    sf::VideoMode vmode(640, 480, 32);
    __LoadVideoModeFromConfigs(vmode);
    game_window = new sf::RenderWindow(vmode, __LoadTitleFromConfigs());
    game_window->setFramerateLimit(__LoadFrameRateFromConfigs());
    game_window->setMouseCursorVisible(false);
    game_window->setVerticalSyncEnabled(__LoadVSYNCFromConfigs());
    return self;
}

VALUE rb_Graphics_stop(VALUE self)
{
    GRAPHICS_PROTECT
    if(InsideGraphicsUpdate) // Prevent a Thread from calling stop during the Graphics.update process
        return self;
    game_window->close();
    delete game_window;
    game_window = nullptr;
}

VALUE rb_Graphics_update(VALUE self)
{
    GRAPHICS_PROTECT
    if(InsideGraphicsUpdate) // Prevent a Thread from calling Graphics.update during Graphics.update process
        return self;
    InsideGraphicsUpdate = true;
    /* Graphics.update real process */
    GraphicUpdateMessage* message = 
        reinterpret_cast<GraphicUpdateMessage*>(rb_thread_call_without_gvl(__Graphics_Update_Internal, (void*)&Graphics_stack, NULL, NULL));
    if(message != nullptr)
        return __Graphics_Update_RaiseError(self, message);
    /* End of Graphics.update process */
    InsideGraphicsUpdate = false;
    return self;
}

void* __Graphics_Update_Internal(void* data)
{
    GraphicUpdateMessage* message = nullptr;
    if(game_window->isOpen())
    {
        /* Draw Processing */
        game_window->clear();
        __Graphics_Update_Draw((std::vector<CDrawable_Element*>*)data);
        game_window->display();
        /* Message Processing */
        __Graphics_Update_Process_Event(message);
    }
    else
    {
        message = new GraphicUpdateMessage();
        message->errorObject = rb_eStoppedGraphics;
        message->message = "Game Window was closed during Graphics.update by a unknow cause...";
    }
    return message;
}

void __Graphics_Update_Process_Event(GraphicUpdateMessage*& message)
{
    if(message != nullptr) // We'll try not to cause other error during this
        return;
    sf::Event event;
    while(game_window->pollEvent(event))
    {
        switch(event.type)
        {
            case sf::Event::Closed:
                message = new GraphicUpdateMessage();
                message->errorObject = rb_eClosedWindow;
                message->message = "Game Window has been closed by user.";
                return;
            
        }
    }
}

VALUE __Graphics_Update_RaiseError(VALUE self, GraphicUpdateMessage* message)
{
    /* If the error is ClosedWindowError, we manage the window closing 
     * When @on_close is defined to a proc, @on_close can decide if the window closing is allowed or not
     * or do things before closing the window
     * That's a kind of rescue process 
     */
    if(message->errorObject == rb_eClosedWindow)
    {
        VALUE closeHandle = rb_iv_get(self, "@on_close");
        if(closeHandle != Qnil)
        {
            VALUE handleClass = rb_class_of(closeHandle);
            if(handleClass == rb_cProc)
                if(rb_proc_call(closeHandle, rb_ary_new()) == Qfalse)
                {
                    InsideGraphicsUpdate = false;
                    return self; /* If the proc returns false we doesn't show the exception */
                }
        }
        game_window->close();
        delete game_window;
        game_window = nullptr;
    }
    /* We raise the message */
    InsideGraphicsUpdate = false;
    rb_raise(message->errorObject, "%s", message->message);
    return self;
}

void __Graphics_Update_Draw(std::vector<CDrawable_Element*>* stack)
{
    bool was_viewport = true;
    for(auto element = stack->begin();element != stack->end(); element++)
    {
        if(was_viewport && !(*element)->isViewport())
            game_window->setView(game_window->getDefaultView());
        else
            was_viewport = (*element)->isViewport();
        (*element)->draw(*game_window);
    }
}

void __LoadVideoModeFromConfigs(sf::VideoMode& vmode)
{
    ID swidth = rb_intern("ScreenWidth");
    ID sheight = rb_intern("ScreenHeight");
    long max_width, max_height = 0xFFFFFF;
    long pixdepth = 32;
    std::vector<sf::VideoMode> modes = sf::VideoMode::getFullscreenModes();
    /* If there's a fullscreen mode */
    if(modes.size() > 0)
    {
        max_width = modes[0].width;
        max_height = modes[0].height;
        vmode.bitsPerPixel = modes[0].bitsPerPixel;
    }
    /* Adjust Width */
    if(rb_const_defined(rb_mConfig, swidth))
        vmode.width = normalize_long(rb_num2long(rb_const_get(rb_mConfig, swidth)), 160, max_width);
    /* Adjust Height */
    if(rb_const_defined(rb_mConfig, sheight))
        vmode.height = normalize_long(rb_num2long(rb_const_get(rb_mConfig, sheight)), 144, max_height);
}

const sf::String __LoadTitleFromConfigs()
{
    ID title = rb_intern("Title");
    if(rb_const_defined(rb_mConfig, title))
    {
        VALUE str_title = rb_const_get(rb_mConfig, title);
        rb_check_type(str_title, T_STRING);
        std::string str(RSTRING_PTR(str_title));
        return sf::String::fromUtf8(str.begin(), str.end());
    }
    return sf::String("LiteRGSS");
}

unsigned int __LoadFrameRateFromConfigs()
{
    ID framerate = rb_intern("FrameRate");
    if(rb_const_defined(rb_mConfig, framerate))
        return normalize_long(rb_num2long(rb_const_get(rb_mConfig, framerate)), 1, 120);
    return 60;
}

bool __LoadVSYNCFromConfigs()
{
    ID vsync = rb_intern("Vsync");
    if(rb_const_defined(rb_mConfig, vsync))
        return RTEST(rb_const_get(rb_mConfig, vsync));
    return true;
}

void __Graphics_Bind(CDrawable_Element* element)
{
    element->setOriginStack(&Graphics_stack);
}