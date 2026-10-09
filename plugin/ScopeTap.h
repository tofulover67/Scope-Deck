#pragma once

#include "ofxsImageEffect.h"

class ScopeTapFactory : public OFX::PluginFactoryHelper<ScopeTapFactory>
{
public:
    ScopeTapFactory();
    virtual void load() {}
    virtual void unload() {}
    virtual void describe(OFX::ImageEffectDescriptor& p_Desc);
    virtual void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context);
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context);
};
