#include "PluginEditor.h"
#include "BinaryData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr int panelWidth=872, panelHeight=374, settingsHeight=68;
const juce::Colour amber(0xfff2be55),coral(0xffef8f8b),blue(0xff5ec6ed),mint(0xff6ddab7),green(0xffa5d26d),violet(0xffb3a4ee),silver(0xffbfcad1);
const char* const effectNames[]={"Off","Reverb","Delay","TranceGate","Chorus","Phaser","Flanger","Distortion","Bitcrusher","Leslie","Talkbox"};
const char* const waveNames[]={"Saw","Square","Pulse","SawPulse","DblCos","Reso","ResoSqu","DblSaw"};
juce::Image resource(int id){
    static const auto images=[] {
        std::array<juce::Image,21> result;
        for(int i=0;i<21;++i){int size=0;const auto name="bmp00"+juce::String(i+128)+"_png";
            if(const auto* bytes=PlastiCZAssets::getNamedResource(name.toRawUTF8(),size))result[static_cast<size_t>(i)]=juce::ImageCache::getFromMemory(bytes,size);}
        return result;
    }();
    return images[static_cast<size_t>(id-128)];
}
juce::RangedAudioParameter& parameter(PlastiCZProcessor& p,int index){return *p.parameters.getParameter(PlastiCZProcessor::parameterID(index));}
float raw(PlastiCZProcessor& p,int index){return p.parameters.getRawParameterValue(PlastiCZProcessor::parameterID(index))->load();}
int effectIndex(float v){return juce::jlimit(0,10,static_cast<int>(v*10.989999771118164f));}
int waveIndex(float v){return juce::jlimit(0,7,static_cast<int>(v*7.989999771118164f));}

// New Classic labels use a small pixel alphabet, without altering original assets.
void pixelText(juce::Graphics& g,const juce::String& text,juce::Rectangle<int> bounds,bool centred=false){
    static constexpr const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/";
    static constexpr unsigned char glyphs[][7]={
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,25,21,19,19,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},{14,17,19,21,25,17,14},
        {4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},
        {31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
        {1,1,2,4,8,16,16}};
    const auto label=text.toUpperCase();int x=centred?bounds.getCentreX()-(label.length()*6-1)/2:bounds.getX();
    const int y=bounds.getCentreY()-3;
    for(auto c:label){
        const auto* found=c<128?std::strchr(alphabet,static_cast<int>(c)):nullptr;
        if(found)for(int row=0;row<7;++row)for(int col=0;col<5;++col)
            if(glyphs[found-alphabet][row]&(16>>col))g.fillRect(x+col,y+row,1,1);
        x+=6;
    }
}
void classicBevel(juce::Graphics& g,juce::Rectangle<int> bounds,bool down){
    g.setGradientFill(juce::ColourGradient(juce::Colour(down?0xff8e929e:0xffe3e5e9),0,float(bounds.getY()),
        juce::Colour(down?0xffd0d3da:0xff9c9faa),0,float(bounds.getBottom()),false));g.fillRect(bounds);
    g.setColour(juce::Colour(0xff434953));g.drawRect(bounds,1);
    g.setColour(juce::Colour(down?0xff737784:0xfff5f5f7));g.drawHorizontalLine(bounds.getY()+1,float(bounds.getX()+1),float(bounds.getRight()-1));
    g.drawVerticalLine(bounds.getX()+1,float(bounds.getY()+1),float(bounds.getBottom()-1));
    g.setColour(juce::Colour(0xff757986));g.drawHorizontalLine(bounds.getBottom()-2,float(bounds.getX()+2),float(bounds.getRight()-1));
}

class PanelLook final:public juce::LookAndFeel_V4 {
public:
    bool classic=true;
    PanelLook(){
        setColour(juce::PopupMenu::backgroundColourId,juce::Colour(0xff20262e));setColour(juce::PopupMenu::textColourId,juce::Colour(0xffeef3f6));
        setColour(juce::PopupMenu::highlightedBackgroundColourId,juce::Colour(0xff386d60));setColour(juce::PopupMenu::highlightedTextColourId,juce::Colours::white);
        setColour(juce::TextButton::buttonColourId,juce::Colour(0xff28333c));setColour(juce::TextButton::textColourOffId,juce::Colour(0xffe7edf2));
    }
    juce::Font getComboBoxFont(juce::ComboBox& box) override{
        if(box.getComponentID()=="plasticz_p28")return juce::Font(classic?9.0f:9.8f,juce::Font::bold);
        if(box.getComponentID()=="preset-menu")return juce::Font(classic?13.0f:12.5f,juce::Font::bold);
        return juce::Font(12.0f);
    }
    void positionComboBoxText(juce::ComboBox& box,juce::Label& label) override{
        const bool effect=box.getComponentID()=="plasticz_p28";label.setBounds(effect?3:7,0,box.getWidth()-(effect?14:23),box.getHeight());label.setFont(getComboBoxFont(box));
        label.setColour(juce::Label::textColourId,box.getComponentID()=="preset-menu"&&classic?juce::Colour(0xff155047):juce::Colour(0xffedf2f6));
    }
    void drawLabel(juce::Graphics& g,juce::Label& label) override{
        if(classic&&label.getParentComponent()!=nullptr&&label.getParentComponent()->getComponentID()=="interface-mode"){
            g.setColour(juce::Colour(0xff343944));pixelText(g,label.getText(),label.getLocalBounds());return;}
        LookAndFeel_V4::drawLabel(g,label);
    }
    void drawComboBox(juce::Graphics& g,int w,int h,bool,int,int,int,int,juce::ComboBox& box) override{
        const auto id=box.getComponentID();
        if(classic&&id=="interface-mode"){
            classicBevel(g,{0,0,w,h},false);g.setColour(juce::Colour(0xff434953));
            for(int row=0;row<4;++row)g.fillRect(w-14+row,h/2-2+row,7-row*2,1);return;}
        if(id=="preset-menu"){
            if(!classic){g.setColour(juce::Colour(0xff18362f));g.fillRoundedRectangle(0,0,float(w),float(h),4);g.setColour(mint.withAlpha(0.35f));g.drawRoundedRectangle(0.5f,0.5f,w-1.0f,h-1.0f,4,1);}
        }else{
            g.setColour(classic&&id=="plasticz_p28"?juce::Colour(0xff777b94):juce::Colour(0xff29323e));g.fillRoundedRectangle(0,0,float(w),float(h),classic?1.0f:3.0f);
            g.setColour(juce::Colour(0xff82949f).withAlpha(0.55f));g.drawRoundedRectangle(0.5f,0.5f,w-1.0f,h-1.0f,classic?1.0f:3.0f,0.8f);
        }
        g.setColour(id=="preset-menu"&&classic?juce::Colour(0xff1a6554):juce::Colour(0xffb3c9d1));
        juce::Path arrow;const float x=w-9.0f,y=h*0.5f;arrow.startNewSubPath(x-3,y-1.5f);arrow.lineTo(x,y+1.5f);arrow.lineTo(x+3,y-1.5f);g.strokePath(arrow,juce::PathStrokeType(1.2f));
    }
    void drawToggleButton(juce::Graphics& g,juce::ToggleButton& button,bool over,bool down) override{
        if(classic){
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(resource(147),1,5,16,16,0,button.getToggleState()?16:0,16,16);
            g.setColour(juce::Colour(0xff343944));pixelText(g,button.getButtonText(),{25,0,button.getWidth()-25,button.getHeight()});
            if(over||down||button.hasKeyboardFocus(true)){g.setColour(juce::Colour(0xff5a616e));g.drawRect(0,4,18,18,1);}return;}
        const bool on=button.getToggleState();g.setColour(on?mint:juce::Colour(0xff52616c));g.fillRoundedRectangle(1,5,30,16,8);
        g.setColour(juce::Colour(0xffedf3f5));g.fillEllipse(on?18.0f:4.0f,8,10,10);
        g.setColour(over||down?juce::Colours::white:juce::Colour(0xffc1ced5));g.setFont(juce::Font(11.5f,juce::Font::bold));
        g.drawText(button.getButtonText(),39,0,button.getWidth()-39,button.getHeight(),juce::Justification::centredLeft);
    }
    void drawButtonBackground(juce::Graphics& g,juce::Button& button,const juce::Colour&,bool over,bool down) override{
        if(classic){classicBevel(g,button.getLocalBounds(),down);
            if(over||button.hasKeyboardFocus(true)){g.setColour(juce::Colour(0xffe9ebee));g.drawRect(button.getLocalBounds().reduced(2),1);}return;}
        const auto bounds=button.getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(down?mint.withAlpha(0.28f):juce::Colour(over?0xff354654:0xff283640));g.fillRoundedRectangle(bounds,4);
        g.setColour(over||button.hasKeyboardFocus(true)?mint:mint.withAlpha(0.35f));g.drawRoundedRectangle(bounds,4,1);
    }
    void drawButtonText(juce::Graphics& g,juce::TextButton& button,bool,bool down) override{
        if(classic){g.setColour(juce::Colour(0xff343944));pixelText(g,button.getButtonText(),button.getLocalBounds().translated(down?1:0,down?1:0),true);return;}
        g.setColour(juce::Colour(0xffd4e2e6));g.setFont(juce::Font(10.5f,juce::Font::bold));
        g.drawText(button.getButtonText(),button.getLocalBounds().reduced(4,0),juce::Justification::centred);
    }
};

class OriginalSlider final:public juce::Component,public juce::SettableTooltipClient {
public:
    OriginalSlider(PlastiCZProcessor& p,int index,bool horizontal,int bitmap,juce::Colour tint,PanelLook& l,const juce::String& name)
      :parameterRef(parameter(p,index)),attachment(parameterRef,[this](float v){value=parameterRef.convertTo0to1(v);repaint();if(onChange)onChange();},nullptr),
       isHorizontal(horizontal),imageId(bitmap),colour(tint),look(l){
        setComponentID(PlastiCZProcessor::parameterID(index));setName(name);setTitle(name);setTooltip(name);setWantsKeyboardFocus(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);attachment.sendInitialUpdate();
    }
    void paint(juce::Graphics& g) override{
        const int thumbHeight=isHorizontal?11:13;const float travel=float((isHorizontal?getWidth():getHeight())-(isHorizontal?13:thumbHeight));
        const float at=travel*(isHorizontal?value:1.0f-value);
        if(look.classic){g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);g.drawImage(resource(imageId),isHorizontal?juce::roundToInt(at):0,isHorizontal?0:juce::roundToInt(at),13,thumbHeight,0,isMouseOverOrDragging()?thumbHeight:0,13,thumbHeight);}
        else{
            const float cx=getWidth()*0.5f,cy=getHeight()*0.5f;g.setColour(juce::Colour(0xff101922));
            if(isHorizontal)g.fillRoundedRectangle(6,cy-1.5f,getWidth()-12.0f,3,1.5f);else g.fillRoundedRectangle(cx-1.5f,6,3,getHeight()-12.0f,1.5f);
            g.setColour(colour.withAlpha(0.48f));if(isHorizontal)g.fillRoundedRectangle(6,cy-1,at,2,1);else g.fillRoundedRectangle(cx-1,at+6,2,travel-at,1);
            const auto thumb=isHorizontal?juce::Rectangle<float>(at+1,0,11,11):juce::Rectangle<float>(1,at+1,11,11);
            g.setColour(colour.darker(0.5f));g.fillRoundedRectangle(thumb.translated(0,1),2);g.setColour(isMouseOverOrDragging()?colour.brighter(0.2f):colour);g.fillRoundedRectangle(thumb,2);
            g.setColour(juce::Colours::white.withAlpha(0.6f));if(isHorizontal)g.drawVerticalLine(juce::roundToInt(at+6),3,8);else g.drawHorizontalLine(juce::roundToInt(at+6),3,10);
        }
        if(hasKeyboardFocus(true)){g.setColour(colour);g.drawRect(getLocalBounds(),1);}
    }
    void mouseDown(const juce::MouseEvent& e) override{gestureActive=true;attachment.beginGesture();drag(e);}
    void mouseDrag(const juce::MouseEvent& e) override{drag(e);}
    void mouseUp(const juce::MouseEvent&) override{if(gestureActive){attachment.endGesture();gestureActive=false;}}
    void mouseDoubleClick(const juce::MouseEvent&) override{const auto initial=parameterRef.convertFrom0to1(parameterRef.getDefaultValue());if(gestureActive)attachment.setValueAsPartOfGesture(initial);else attachment.setValueAsCompleteGesture(initial);}
    void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails& w) override{change(value+(w.deltaY!=0?w.deltaY:w.deltaX)*0.1f);}
    bool keyPressed(const juce::KeyPress& k) override{
        const auto c=k.getKeyCode();if(c==juce::KeyPress::leftKey||c==juce::KeyPress::downKey){change(value-0.01f);return true;}
        if(c==juce::KeyPress::rightKey||c==juce::KeyPress::upKey){change(value+0.01f);return true;}return false;
    }
    void mouseEnter(const juce::MouseEvent&) override{repaint();}void mouseExit(const juce::MouseEvent&) override{repaint();}
    std::function<void()> onChange;
private:
    void change(float v){attachment.setValueAsCompleteGesture(parameterRef.convertFrom0to1(juce::jlimit(0.0f,1.0f,v)));}
    void drag(const juce::MouseEvent& e){const float travel=float((isHorizontal?getWidth():getHeight())-13);float v=((isHorizontal?e.position.x:e.position.y)-6.5f)/travel;if(!isHorizontal)v=1-v;attachment.setValueAsPartOfGesture(parameterRef.convertFrom0to1(juce::jlimit(0.0f,1.0f,v)));}
    juce::RangedAudioParameter& parameterRef;juce::ParameterAttachment attachment;float value=0;bool isHorizontal;int imageId;juce::Colour colour;PanelLook& look;bool gestureActive=false;
};

class WaveButton final:public juce::Button {
public:
    WaveButton(int wave,int bank,PanelLook& l):Button(waveNames[wave]),index(wave),group(bank),look(l){setTooltip(waveNames[wave]);}
    void paintButton(juce::Graphics& g,bool over,bool) override{
        if(look.classic){g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);g.drawImage(resource(137),0,0,getWidth(),getHeight(),20*(index%4)+100*group,20*(index/4)+(getToggleState()?36:0),16,16);
            if(over){g.setColour(juce::Colours::white.withAlpha(0.4f));g.drawRect(getLocalBounds(),1);}return;}
        g.setColour(getToggleState()?blue.withAlpha(0.3f):juce::Colour(0xff15232f));g.fillRoundedRectangle(getLocalBounds().toFloat(),2);
        g.setColour(getToggleState()?blue:juce::Colour(0xff7194a7));if(over||getToggleState())g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f),2,0.8f);
        juce::Path path;for(int x=0;x<=24;++x){const float t=float(x)/24;float y=0;switch(index){
            case 0:y=t;break;case 1:y=t<0.5f?0.95f:0.05f;break;case 2:y=t<0.25f?0.95f:0.05f;break;case 3:y=t<0.5f?t*2:0.05f;break;
            case 4:y=0.5f-0.45f*std::cos(t*juce::MathConstants<float>::twoPi*2);break;
            case 5:y=0.5f+std::sin(t*juce::MathConstants<float>::twoPi*3)*(1-t)*0.5f;break;
            case 6:y=t<0.5f?0.7f:0.15f+0.15f*std::sin(t*25);break;default:y=std::fmod(t*2,1.0f);break;}
            const float px=2+t*(getWidth()-4),py=getHeight()-2-y*(getHeight()-4);if(x==0)path.startNewSubPath(px,py);else path.lineTo(px,py);}
        g.strokePath(path,juce::PathStrokeType(1.1f));
    }
private:int index,group;PanelLook& look;
};
class WaveGroup final:public juce::Component {
public:
    WaveGroup(PlastiCZProcessor& p,int index,int bank,PanelLook& look):attachment(parameter(p,index),[this](float v){update(v);},nullptr){
        setComponentID(PlastiCZProcessor::parameterID(index));setName("Oscillator "+juce::String(index<10?1:2)+(index%2==0?" wave A":" wave B"));
        for(int i=0;i<8;++i){auto b=std::make_unique<WaveButton>(i,bank,look);b->setComponentID(getComponentID()+"_wave_"+juce::String(i));b->setBounds((i%4)*20,(i/4)*20,16,16);
            b->onClick=[this,i]{if(!buttons[size_t(i)]->getToggleState())attachment.setValueAsCompleteGesture(float(i)*0.1428571492433548f);};addAndMakeVisible(*b);buttons.push_back(std::move(b));}attachment.sendInitialUpdate();
    }
private:
    void update(float v){const auto selected=waveIndex(v);for(int i=0;i<int(buttons.size());++i)buttons[size_t(i)]->setToggleState(i==selected,juce::dontSendNotification);}
    std::vector<std::unique_ptr<WaveButton>> buttons;juce::ParameterAttachment attachment;
};
class BitmapButton final:public juce::Button {
public:
    BitmapButton(const juce::String& name,int bitmap,PanelLook& l):Button(name),imageId(bitmap),look(l){setTooltip(name);}
    void paintButton(juce::Graphics& g,bool over,bool down) override{
        if(look.classic){g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);g.drawImage(resource(imageId),0,0,getWidth(),getHeight(),0,down?16:0,16,16);}
        else{g.setColour(over?juce::Colour(0xff566574):juce::Colour(0xff344351));g.fillRoundedRectangle(getLocalBounds().toFloat(),2);g.setColour(juce::Colour(0xffb6cbd1));
            if(imageId==145||imageId==146){juce::Path p;const float y=imageId==145?10.0f:6.0f,d=imageId==145?-1.0f:1.0f;p.startNewSubPath(4,y);p.lineTo(8,y+4*d);p.lineTo(12,y);g.strokePath(p,juce::PathStrokeType(1.2f));}
            else{g.drawRoundedRectangle(4,4,8,8,1,1);g.drawHorizontalLine(7,5,11);}}
    }
private:int imageId;PanelLook& look;
};
class RenameButton final:public juce::Button {
public:
    explicit RenameButton(PanelLook& l):Button("Rename preset"),look(l){setTooltip("Rename preset");setComponentID("rename-preset");}
    void paintButton(juce::Graphics& g,bool over,bool) override{
        if(!look.classic||over){g.setColour(mint);juce::Path p;p.startNewSubPath(2,4);p.lineTo(7,8.5f);p.lineTo(2,13);p.closeSubPath();g.fillPath(p);}
    }
private:PanelLook& look;
};
class SettingsButton final:public juce::Button {
public:
    explicit SettingsButton(PanelLook& l):Button("Settings"),look(l){
        setComponentID("settings-toggle");setTooltip("Show settings");setClickingTogglesState(true);
    }
    void paintButton(juce::Graphics& g,bool over,bool down) override{
        const bool on=getToggleState();
        if(look.classic){
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(resource(147),0,0,24,24,0,on||down?16:0,16,16);
            // Whole logical pixels keep the new icon in the original sprite style.
            constexpr const char* pixels[]={
                "000111000","010111010","011101110","111000111","110000011",
                "111000111","011101110","010111010","000111000"};
            g.setColour(juce::Colour(0xff374047));
            // Leave the original lamp at the bottom of the button unobstructed.
            for(int y=0;y<9;++y)for(int x=0;x<9;++x)if(pixels[y][x]=='1')g.fillRect(x+7,y+4,1,1);
            if(over){g.setColour(juce::Colours::white.withAlpha(0.5f));g.drawRect(getLocalBounds(),1);}
        }else{
            g.setColour(on?mint.withAlpha(0.16f):juce::Colour(0xff26333e));g.fillRoundedRectangle(getLocalBounds().toFloat(),4);
            juce::Path gear;
            for(int i=0;i<32;++i){const float angle=i*juce::MathConstants<float>::twoPi/32;
                const float radius=i%4==1||i%4==2?8.2f:6.3f;const float x=12+std::cos(angle)*radius,y=12+std::sin(angle)*radius;
                if(i==0)gear.startNewSubPath(x,y);else gear.lineTo(x,y);}
            gear.closeSubPath();gear.addEllipse(9,9,6,6);gear.setUsingNonZeroWinding(false);
            g.setColour(on?mint:(over||down?juce::Colours::white:silver));g.fillPath(gear);
        }
        if(hasKeyboardFocus(true)){g.setColour(on?mint:silver);g.drawRect(getLocalBounds(),1);}
    }
private:PanelLook& look;
};
class BitmapToggle final:public juce::ToggleButton {
public:
    BitmapToggle(PlastiCZProcessor& p,int index,PanelLook& l,bool compact,const juce::String& text):ToggleButton(text),attachment(parameter(p,index),[this](float v){setToggleState(v>=0.5f,juce::dontSendNotification);repaint();},nullptr),look(l),small(compact),imageId(index==37?147:148){
        setComponentID(PlastiCZProcessor::parameterID(index));setTooltip(text);onClick=[this]{attachment.setValueAsCompleteGesture(getToggleState()?1.0f:0.0f);};attachment.sendInitialUpdate();}
    void paintButton(juce::Graphics& g,bool over,bool down) override{
        if(!small){ToggleButton::paintButton(g,over,down);return;}
        if(look.classic)g.drawImage(resource(imageId),0,0,16,16,0,getToggleState()?16:0,16,16);
        else{g.setColour(getToggleState()?amber:juce::Colour(0xff344351));g.fillRoundedRectangle(0,0,16,16,2);g.setColour(getToggleState()?juce::Colour(0xff48371d):silver);g.drawRect(5,5,6,6,1);}
    }
private:juce::ParameterAttachment attachment;PanelLook& look;bool small;int imageId;
};
}

struct PlastiCZEditor::Interface final:public juce::Component,private juce::Timer {
    explicit Interface(PlastiCZProcessor& p):processor(p),bypass(p,36,look,true,"Bypass effects"),oversampling(p,37,look,false,"4x oversampling"),
      backOversampling(p,37,look,true,"4x oversampling"),previous("Previous preset",145,look),next("Next preset",146,look),
      loadPreset("Load preset",147,look),savePreset("Save preset",147,look),loadBank("Load bank",147,look),saveBank("Save bank",147,look),setup("Setup / front panel",147,look),rename(look),settings(look),
      fxAttachment(parameter(p,28),[this](float v){effect.setSelectedId(effectIndex(v)+1,juce::dontSendNotification);updateEffectLabels();},nullptr){
        look.classic=processor.isClassicEditor();setLookAndFeel(&look);
        effect.setComponentID("plasticz_p28");effect.setName("Effect type");effect.setTooltip("Choose an effect");for(int i=0;i<11;++i)effect.addItem(effectNames[i],i+1);
        effect.onChange=[this]{if(effect.getSelectedId()>0)fxAttachment.setValueAsCompleteGesture((effect.getSelectedId()-1)/10.0f);};addFront(effect,{356,242,81,15});fxAttachment.sendInitialUpdate();
        preset.setComponentID("preset-menu");preset.setName("Current preset");preset.onChange=[this]{if(preset.getSelectedId()>0)processor.setCurrentProgram(preset.getSelectedId()-1);};addFront(preset,{121,23,211,31});
        addFront(rename,{111,29,9,17});rename.onClick=[this]{renamePreset();};
        addFront(previous,{339,21,16,16});addFront(next,{339,41,16,16});previous.onClick=[this]{processor.setCurrentProgram((processor.getCurrentProgram()+127)%128);};next.onClick=[this]{processor.setCurrentProgram((processor.getCurrentProgram()+1)%128);};
        addFront(loadPreset,{407,21,16,16});addFront(savePreset,{427,21,16,16});addFront(loadBank,{407,41,16,16});addFront(saveBank,{427,41,16,16});
        loadPreset.onClick=loadBank.onClick=[this]{chooseImport();};savePreset.onClick=[this]{chooseExport(false);};saveBank.onClick=[this]{chooseExport(true);};
        addFront(bypass,{482,41,16,16});setup.setBounds(502,41,16,16);addAndMakeVisible(setup);setup.onClick=[this]{setBackPanel(!backPanel);};
        backOversampling.setComponentID("plasticz_p37-back");backOversampling.setBounds(502,66,16,16);addChildComponent(backOversampling);
        const int envelopeX[]={105,137,169,201,249,281};const char* envelopeNames[]={"Attack","Decay","Sustain","Release","Depth","Velocity sensitivity"};
        for(int i=0;i<6;++i){addSlider(i,{envelopeX[i],148,13,61},false,129,amber,juce::String("DCW ")+envelopeNames[i]);addSlider(20+i,{envelopeX[i],285,13,61},false,133,coral,juce::String("Amplifier ")+envelopeNames[i]);}
        for(int i=0;i<4;++i){auto w=std::make_unique<WaveGroup>(p,8+i,i%2,look);addFront(*w,{i%2==0?346:446,i<2?102:170,76,36});waveGroups.push_back(std::move(w));
            addSlider(12+i,{594,101+(i%2)*24+(i/2)*68,60,11},true,i%2==0?130:131,mint,"Oscillator "+juce::String(i/2+1)+(i%2==0?" harmonic":" fine tuning"));}
        const char* mixNames[]={"Ring modulation","Oscillator 1 level","Oscillator 2 level"};for(int i=0;i<3;++i)addSlider(16+i,{703+i*32,148,13,61},false,132,green,mixNames[i]);
        for(int i=0;i<3;++i)addSlider(29+i,{356+i*32,285,13,61},false,134,violet,i==0?juce::String("Effect mix"):"Effect parameter "+juce::String(i));
        const char* miscNames[]={"Pitch bend range","Portamento speed","Vibrato speed","Voice count"};for(int i=0;i<4;++i)addSlider(32+i,{i<2?614:740,i%2==0?238:262,60,11},true,i%2==0?135:136,silver,miscNames[i]);
        for(auto* label:{&fxMixLabel,&fxOneLabel,&fxTwoLabel}){label->setFont(juce::Font(8.5f,juce::Font::bold));label->setJustificationType(juce::Justification::centred);addFront(*label,{});label->setInterceptsMouseClicks(false,false);}
        fxMixLabel.setBounds(343,269,39,15);fxOneLabel.setBounds(377,269,39,15);fxTwoLabel.setBounds(410,269,39,15);
        mode.setComponentID("interface-mode");mode.setName("Interface style");mode.addItem("Classic",1);mode.addItem("Modern",2);mode.setSelectedId(look.classic?1:2,juce::dontSendNotification);
        mode.onChange=[this]{const bool classic=mode.getSelectedId()==1;processor.setClassicEditor(classic);applyStyle(classic);};mode.setBounds(134,386,112,27);addChildComponent(mode);
        oversampling.setBounds(274,386,171,27);addChildComponent(oversampling);importButton.setBounds(492,386,135,28);addChildComponent(importButton);panicButton.setBounds(652,386,144,28);addChildComponent(panicButton);
        importButton.onClick=[this]{chooseImport();};panicButton.onClick=[this]{processor.requestPanic();};status.setBounds(66,418,738,15);status.setFont(juce::Font(10.0f));status.setColour(juce::Label::textColourId,juce::Colour(0xff93a5af));addChildComponent(status);
        settings.setBounds(782,334,24,24);addAndMakeVisible(settings);settings.onClick=[this]{
            drawerOpen=settings.getToggleState();settings.setTooltip(drawerOpen?"Hide settings":"Show settings");
            for(auto* c:std::array<juce::Component*,5>{&mode,&oversampling,&importButton,&panicButton,&status})c->setVisible(drawerOpen);
            if(onSettingsChanged)onSettingsChanged();repaint();};
        lastMidiActivity=processor.midiActivity();refresh();updateEffectLabels();startTimerHz(30);
    }
    ~Interface() override{stopTimer();setLookAndFeel(nullptr);}
    int logicalHeight() const{return panelHeight+(drawerOpen?settingsHeight:0);}
    std::function<void()> onSettingsChanged;
    void refresh(){preset.clear(juce::dontSendNotification);for(int i=0;i<processor.getNumPrograms();++i)preset.addItem(juce::String(i+1).paddedLeft('0',3)+"  "+processor.getProgramName(i),i+1);
        preset.setSelectedId(processor.getCurrentProgram()+1,juce::dontSendNotification);preset.setTooltip(processor.getProgramName(processor.getCurrentProgram()));applyStyle(processor.isClassicEditor());
        if(processor.lastStateError().isNotEmpty())status.setText(processor.lastStateError(),juce::dontSendNotification);}
    void paint(juce::Graphics& g) override{
        if(look.classic){g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);g.drawImageAt(resource(backPanel?144:128),0,0);}else paintModern(g);
        if(!backPanel){paintMeters(g);paintReadouts(g);}
        if(drawerOpen)paintSettings(g);
    }
private:
    void paintSettings(juce::Graphics& g){
        if(look.classic){
            // Extend the original rack edges and its unused brushed-metal area.
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(resource(128),0,panelHeight,47,settingsHeight,0,250,47,settingsHeight);
            g.drawImage(resource(128),47,panelHeight,779,settingsHeight,550,288,272,settingsHeight);
            g.drawImage(resource(128),826,panelHeight,46,settingsHeight,826,250,46,settingsHeight);
            g.setColour(juce::Colour(0xff555b69));g.drawHorizontalLine(panelHeight,47.0f,826.0f);
            g.setColour(juce::Colour(0xffd5d7de));g.drawHorizontalLine(panelHeight+1,47.0f,826.0f);
            g.setColour(juce::Colour(0xff343944));pixelText(g,"Interface",{66,386,65,27});
        }else{
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff26333e),0,float(panelHeight),juce::Colour(0xff19232d),0,float(panelHeight+settingsHeight),false));
            g.fillRect(0,panelHeight,panelWidth,settingsHeight);panel(g,{66,381,740,38},mint);
            g.setColour(silver);g.setFont(juce::Font(10.5f,juce::Font::bold));g.drawText("INTERFACE",74,386,60,27,juce::Justification::centredLeft);
        }
    }
    void addFront(juce::Component& c,juce::Rectangle<int> b){c.setBounds(b);addAndMakeVisible(c);frontControls.push_back(&c);}
    void addSlider(int i,juce::Rectangle<int> b,bool horizontal,int bitmap,juce::Colour tint,const juce::String& name){auto s=std::make_unique<OriginalSlider>(processor,i,horizontal,bitmap,tint,look,name);s->onChange=[this]{repaint();};addFront(*s,b);sliders.push_back(std::move(s));}
    void applyStyle(bool classic){look.classic=classic;mode.setSelectedId(classic?1:2,juce::dontSendNotification);for(auto* c:frontControls)c->sendLookAndFeelChange();mode.sendLookAndFeelChange();
        status.setColour(juce::Label::textColourId,classic?juce::Colour(0xff343944):juce::Colour(0xff93a5af));updateEffectLabels();repaint();}
    void setBackPanel(bool back){backPanel=back;for(auto* c:frontControls)c->setVisible(!back);backOversampling.setVisible(back);repaint();}
    void updateEffectLabels(){
        static const char* labels[][3]={{"Mix","Par 1","Par 2"},{"Mix","Size","Damp"},{"Mix","Time","Feed"},{"Fade","Pattern","Speed"},{"Mix","Rate","Depth"},{"Mix","Rate","Feed"},{"Mix","Rate","Depth"},{"Mix","Drive","Tone"},{"Mix","Bits","Rate"},{"Mix","Rate","Depth"},{"Mix","Rate","Range"}};
        const auto i=effectIndex(raw(processor,28));juce::Label* target[]={&fxMixLabel,&fxOneLabel,&fxTwoLabel};for(int n=0;n<3;++n){target[n]->setText(labels[i][n],juce::dontSendNotification);
            target[n]->setColour(juce::Label::textColourId,look.classic?juce::Colour(0xffd3d4ff):violet);target[n]->setColour(juce::Label::backgroundColourId,look.classic?juce::Colour(0xff737391):juce::Colour(0xff292c3e));}}
    void timerCallback() override{
        for(int channel=0;channel<2;++channel)meter[size_t(channel)]=std::max(processor.outputPeak(channel),meter[size_t(channel)]*0.89f);
        const auto activity=processor.midiActivity();if(activity!=lastMidiActivity){lastMidiActivity=activity;midiFrames=3;}else if(midiFrames>0)--midiFrames;
        repaint(485,276,48,70);repaint(103,6,23,16);
    }
    void paintMeters(juce::Graphics& g){
        if(look.classic)g.drawImage(resource(143),103,6,23,16,0,midiFrames>0?16:0,23,16);
        else{g.setColour(midiFrames>0?mint:juce::Colour(0xff566370));g.fillRoundedRectangle(108,10,11,4,1);}
        for(int ch=0;ch<2;++ch){const auto db=juce::Decibels::gainToDecibels(juce::jmax(0.00001f,meter[size_t(ch)]),-60.0f);const float amount=juce::jlimit(0.0f,1.0f,(db+48)/48);
            const int x=495+19*ch;if(look.classic){const auto img=resource(ch==0?141:142);g.drawImage(img,x,280,15,63,0,0,15,63);const int h=juce::roundToInt(63*amount);if(h>0)g.drawImage(img,x,343-h,15,h,15,63-h,15,h);}
            else for(int row=0;row<12;++row){g.setColour(row/12.0f<amount?(row>9?coral:violet):juce::Colour(0xff414253));g.fillRoundedRectangle(float(x),338.0f-row*5,14,3,0.8f);}}
    }
    void paintReadouts(juce::Graphics& g){
        g.setColour(look.classic?juce::Colour(0xffd3d3c5):silver);g.setFont(juce::Font(7.3f,juce::Font::bold));
        const int bend=int(raw(processor,32)*11.989999771118164f)+1,voices=int(raw(processor,35)*15.989999771118164f)+1;
        constexpr int porta[]={1,2,4,8,16,32,64};const int glide=porta[juce::jlimit(0,6,int(raw(processor,33)*6.989999771118164f))];
        const float v=raw(processor,34);const auto vibrato=juce::String(int(v*19)+1)+"."+juce::String((int(v*190)+10)%10);
        if(look.classic){
            const auto digit=[&g](int imageId,int n,int x,int y){g.drawImage(resource(imageId),x,y,6,7,0,7*n,6,7);};
            digit(139,bend<10?10:bend/10,603,240);digit(139,bend%10,608,240);
            digit(140,glide<10?glide:glide/10,603,264);digit(140,glide<10?10:glide%10,608,264);
            const int whole=int(v*19)+1;digit(139,whole<10?10:whole/10,721,240);digit(139,whole%10,726,240);digit(139,(int(v*190)+10)%10,734,240);
            digit(140,voices<10?10:voices/10,729,264);digit(140,voices%10,734,264);return;
        }
        g.drawText(juce::String(bend),599,237,15,11,juce::Justification::centredRight);g.drawText(juce::String(glide),599,261,15,11,juce::Justification::centredRight);
        g.drawText(vibrato,718,237,23,11,juce::Justification::centredRight);g.drawText(juce::String(voices),724,261,16,11,juce::Justification::centredRight);
    }
    void panel(juce::Graphics& g,juce::Rectangle<float> b,juce::Colour c){g.setColour(juce::Colour(0xff080f16));g.fillRoundedRectangle(b.translated(0,2),5);
        g.setGradientFill(juce::ColourGradient(c.darker(0.82f),b.getX(),b.getY(),juce::Colour(0xff1d2631),b.getRight(),b.getBottom(),false));g.fillRoundedRectangle(b,5);g.setColour(c.withAlpha(0.35f));g.drawRoundedRectangle(b.reduced(0.5f),5,0.8f);}
    void text(juce::Graphics& g,const juce::String& s,int x,int y,int w,int h,juce::Colour c,float size=8.5f,juce::Justification align=juce::Justification::centredLeft){g.setColour(c);g.setFont(juce::Font(size,juce::Font::bold));g.drawText(s,x,y,w,h,align);}
    void envelope(juce::Graphics& g,int offset,juce::Colour tint,int base){
        const float attack=raw(processor,base),decay=raw(processor,base+1),sustain=raw(processor,base+2),release=raw(processor,base+3),y=float(offset),bottom=y+38;
        g.setColour(tint.withAlpha(0.1f));for(int x=96;x<=226;x+=26)g.drawVerticalLine(x,y,bottom);g.drawHorizontalLine(int(bottom),96,226);
        const float a=102+attack*28,d=a+12+decay*24,r=juce::jmin(228.0f,d+20+release*32);juce::Path p;p.startNewSubPath(96,bottom);p.lineTo(a,y+5);p.lineTo(d,bottom-sustain*30);p.lineTo(r-8,bottom-sustain*30);p.lineTo(r,bottom);g.setColour(tint);g.strokePath(p,juce::PathStrokeType(1.2f));
    }
    void paintModern(juce::Graphics& g){
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff303b46),0,0,juce::Colour(0xff19232d),0,374,false));g.fillRect(0,0,872,374);
        if(backPanel){text(g,"PlastiCZ",82,37,345,52,mint,39);text(g,"NATIVE EDITION",85,91,280,22,silver,12);
            text(g,"Interface restored from the original PlastiCZ resources.",85,156,600,25,silver,13);text(g,"Original synthesizer: Michael Kleps and Markus Feil / reFX",85,190,650,24,silver,12);
            text(g,juce::String::fromUTF8("Factory presets by Hubertus Maaß, Chris Hülsbeck,"),85,218,650,22,silver,12);text(g,juce::String::fromUTF8("Peter Röhrßen and Markus Feil"),85,241,650,22,silver,12);
            text(g,"setup",525,40,65,17,silver,9);text(g,"oversample 4x",525,65,125,19,silver,10);return;}
        panel(g,{106,19,228,39},mint);text(g,"PRESET",70,25,38,26,silver,8);
        text(g,"load",402,5,26,15,silver,8,juce::Justification::centred);text(g,"save",422,5,26,15,silver,8,juce::Justification::centred);
        text(g,"preset",352,21,50,16,silver,8,juce::Justification::centredRight);text(g,"bank",352,41,50,16,silver,8,juce::Justification::centredRight);
        text(g,"fx",469,41,14,16,silver,9);text(g,"setup",524,41,37,16,silver,9);g.setColour(mint);g.fillEllipse(575,24,27,27);g.setColour(mint.darker(0.5f));g.drawEllipse(575,24,27,27,2);
        text(g,"PlastiCZ",620,10,190,50,juce::Colour(0xffeef5f6),35);text(g,"NATIVE",736,57,67,14,mint,9);
        panel(g,{67,87,244,132},amber);panel(g,{67,225,244,131},coral);panel(g,{317,87,224,65},blue);panel(g,{317,155,224,64},blue);panel(g,{548,87,110,65},mint);panel(g,{548,155,110,64},mint);panel(g,{664,87,142,132},green);panel(g,{317,225,142,131},violet);panel(g,{465,225,76,131},violet);panel(g,{548,225,258,62},silver);
        envelope(g,99,amber,0);envelope(g,237,coral,20);text(g,"DCW",77,190,28,18,amber,11);text(g,"AMP",77,327,28,18,coral,11);
        const char* adsr[]={"A","D","S","R"};for(int i=0;i<4;++i){text(g,adsr[i],108+i*32,137,15,11,amber,8);text(g,adsr[i],108+i*32,274,15,11,coral,8);}
        for(int y:{137,274}){text(g,"Depth",242,y,31,11,y==137?amber:coral,8);text(g,"Vel",279,y,23,11,y==137?amber:coral,8);}
        text(g,"OSC1",324,137,38,12,blue,8);text(g,"OSC2",324,205,38,12,blue,8);text(g,"A",327,101,15,16,blue,9);text(g,"B",429,101,15,16,blue,9);text(g,"A",327,169,15,16,blue,9);text(g,"B",429,169,15,16,blue,9);
        for(int y:{101,169}){text(g,"Harm",555,y,36,11,mint,8);text(g,"Fine",555,y+24,36,11,mint,8);}
        text(g,"MIX",675,190,30,17,green,11);text(g,"Ring",698,136,27,12,green,8);text(g,"OSC1",731,136,30,12,green,8);text(g,"OSC2",765,136,30,12,green,8);
        text(g,"FX",326,329,26,17,violet,13);text(g,"Type",326,241,27,17,violet,8);text(g,"LEVEL",480,242,48,18,violet,9);
        text(g,"Bend",557,238,37,11,silver,8);text(g,"Glide",557,262,37,11,silver,8);text(g,"Vibrato",683,238,48,11,silver,8);text(g,"Voices",683,262,38,11,silver,8);
        text(g,"PHASE DISTORTION SYNTHESIZER",565,317,242,17,juce::Colour(0xff718692),10);
    }
    void chooseImport(){
        chooser=std::make_unique<juce::FileChooser>("Import a PlastiCZ preset or bank",juce::File{},"*.fxp;*.fxb;*.chunk;*.rawchunk;*",true);const juce::Component::SafePointer<Interface> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[safe](const juce::FileChooser& c){if(!safe)return;const auto file=c.getResult();if(!file.existsAsFile())return;juce::String error;
            if(safe->processor.importPreset(file,error))safe->status.setText("Imported "+file.getFileName(),juce::dontSendNotification);else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,"Preset could not be imported",error);});
    }
    void renamePreset(){
        const auto index=processor.getCurrentProgram();auto* dialog=new juce::AlertWindow("Rename preset","Enter a name for this preset.",juce::MessageBoxIconType::NoIcon);
        dialog->addTextEditor("name",processor.getProgramName(index));dialog->getTextEditor("name")->setInputRestrictions(31);
        dialog->addButton("Rename",1,juce::KeyPress(juce::KeyPress::returnKey));dialog->addButton("Cancel",0,juce::KeyPress(juce::KeyPress::escapeKey));
        const juce::Component::SafePointer<Interface> safe(this);dialog->enterModalState(true,juce::ModalCallbackFunction::create([safe,dialog,index](int result){
            if(safe&&result==1)safe->processor.changeProgramName(index,dialog->getTextEditorContents("name"));}),true);
    }
    void chooseExport(bool bank){
        chooser=std::make_unique<juce::FileChooser>(bank?"Save PlastiCZ bank":"Save PlastiCZ preset",juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(bank?"PlastiCZ.fxb":"PlastiCZ.fxp"),bank?"*.fxb":"*.fxp",true);
        const juce::Component::SafePointer<Interface> safe(this);chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting,[safe,bank](const juce::FileChooser& c){
            if(!safe)return;const auto chosen=c.getResult();if(chosen==juce::File{})return;const auto file=chosen.withFileExtension(bank?"fxb":"fxp");
            const auto save=[safe,file,bank]{if(!safe)return;juce::String error;
                if(safe->processor.exportPreset(file,bank,error))safe->status.setText("Saved "+file.getFileName(),juce::dontSendNotification);else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,"Preset could not be saved",error);};
            if(file!=chosen&&file.existsAsFile())juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,"Replace existing file?",file.getFileName()+" already exists.","Replace","Cancel",nullptr,juce::ModalCallbackFunction::create([save](int result){if(result==1)save();}));
            else save();});
    }
    PlastiCZProcessor& processor;PanelLook look;juce::ComboBox preset,effect,mode;BitmapToggle bypass,oversampling,backOversampling;
    BitmapButton previous,next,loadPreset,savePreset,loadBank,saveBank,setup;RenameButton rename;SettingsButton settings;juce::TextButton importButton{"Import preset / bank"},panicButton{"Panic / all notes off"};
    juce::Label fxMixLabel,fxOneLabel,fxTwoLabel,status;std::vector<std::unique_ptr<OriginalSlider>> sliders;std::vector<std::unique_ptr<WaveGroup>> waveGroups;std::vector<juce::Component*> frontControls;
    std::unique_ptr<juce::FileChooser> chooser;juce::TooltipWindow tooltips{this,550};juce::ParameterAttachment fxAttachment;bool backPanel=false,drawerOpen=false;std::array<float,2> meter{};std::uint64_t lastMidiActivity=0;int midiFrames=0;
};
PlastiCZEditor::PlastiCZEditor(PlastiCZProcessor& p):AudioProcessorEditor(p),processor(p),interface(std::make_unique<Interface>(p)){
    addAndMakeVisible(*interface);processor.addChangeListener(this);setResizable(true,true);
    interface->onSettingsChanged=[this]{updateLayout();};updateLayout();
}
PlastiCZEditor::~PlastiCZEditor(){processor.removeChangeListener(this);}
void PlastiCZEditor::updateLayout(){
    const int height=interface->logicalHeight(),width=getWidth()>0?getWidth():1090;
    if(auto* constraint=getConstrainer())constraint->setFixedAspectRatio(double(panelWidth)/height);
    setResizeLimits(panelWidth,height,panelWidth*2,height*2);setSize(width,juce::roundToInt(double(width)*height/panelWidth));
}
void PlastiCZEditor::resized(){const int height=interface->logicalHeight();interface->setBounds(0,0,panelWidth,height);interface->setTransform(juce::AffineTransform::scale(float(getWidth())/panelWidth,float(getHeight())/height));}
void PlastiCZEditor::changeListenerCallback(juce::ChangeBroadcaster*){interface->refresh();}
