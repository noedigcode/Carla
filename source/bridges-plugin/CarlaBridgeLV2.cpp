/*
 * Carla LV2 Single Plugin
 * Copyright (C) 2017 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#ifndef BUILD_BRIDGE
# error This file should not be compiled if not building bridge
#endif

#include "engine/CarlaEngineInternal.hpp"
#include "CarlaPlugin.hpp"

#include "CarlaBackendUtils.hpp"
#include "CarlaEngineUtils.hpp"
#include "CarlaLv2Utils.hpp"
#include "CarlaUtils.h"

#include "water/files/File.h"

// --------------------------------------------------------------------------------------------------------------------

CARLA_BACKEND_START_NAMESPACE

class CarlaEngineLV2Single : public CarlaEngine,
                             public Lv2PluginBaseClass<EngineTimeInfo>
{
public:
    CarlaEngineLV2Single(const double sampleRate,
                         const char* const bundlePath,
                         const LV2_Feature* const* const features)
        : Lv2PluginBaseClass(sampleRate, features),
          fPlugin(nullptr),
          fUiName()
    {
        CARLA_SAFE_ASSERT_RETURN(pData->curPluginCount == 0,)
        CARLA_SAFE_ASSERT_RETURN(pData->plugins[0].plugin == nullptr,);

        if (! loadedInProperHost())
            return;

        // xxxxx
        CarlaString binaryDir(bundlePath);
        binaryDir += CARLA_OS_SEP_STR "bin" CARLA_OS_SEP_STR;

        CarlaString resourceDir(bundlePath);
        resourceDir += CARLA_OS_SEP_STR "res" CARLA_OS_SEP_STR;

        pData->bufferSize = fBufferSize;
        pData->sampleRate = sampleRate;
        pData->initTime(nullptr);

        pData->options.processMode         = ENGINE_PROCESS_MODE_BRIDGE;
        pData->options.transportMode       = ENGINE_TRANSPORT_MODE_PLUGIN;
        pData->options.forceStereo         = false;
        pData->options.preferPluginBridges = false;
        pData->options.preferUiBridges     = false;
        init("LV2-Export");

        if (pData->options.resourceDir != nullptr)
            delete[] pData->options.resourceDir;
        if (pData->options.binaryDir != nullptr)
            delete[] pData->options.binaryDir;

        pData->options.binaryDir   = binaryDir.dup();
        pData->options.resourceDir = resourceDir.dup();

        setCallback(_engine_callback, this);

        using water::File;
        const File pluginFile(File::getSpecialLocation(File::currentExecutableFile).withFileExtension("xml"));

        if (! loadProject(pluginFile.getFullPathName().toRawUTF8()))
        {
            carla_stderr2("Failed to init plugin, possible reasons: %s", getLastError());
            return;
        }

        CARLA_SAFE_ASSERT_RETURN(pData->curPluginCount == 1,)

        fPlugin = pData->plugins[0].plugin;
        CARLA_SAFE_ASSERT_RETURN(fPlugin != nullptr,);
        CARLA_SAFE_ASSERT_RETURN(fPlugin->isEnabled(),);

        fPorts.usesTime     = true;
        fPorts.numAudioIns  = fPlugin->getAudioInCount();
        fPorts.numAudioOuts = fPlugin->getAudioOutCount();
        fPorts.numMidiIns   = fPlugin->getMidiInCount();
        fPorts.numMidiOuts  = fPlugin->getMidiOutCount();
        fPorts.numParams    = fPlugin->getParameterCount();

        fPorts.init();

        for (uint32_t i=0; i < fPorts.numParams; ++i)
        {
            fPorts.paramsLast[i] = fPlugin->getParameterValue(i);
            fPorts.paramsOut [i] = fPlugin->isParameterOutput(i);
        }
    }

    ~CarlaEngineLV2Single()
    {
        if (fPlugin != nullptr && fIsActive)
            fPlugin->setActive(false, false, false);

        close();
    }

    bool hasPlugin() noexcept
    {
        return fPlugin != nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // LV2 functions

    void lv2_activate() noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(! fIsActive,);

        resetTimeInfo();

        fPlugin->setActive(true, false, false);
        fIsActive = true;
    }

    void lv2_deactivate() noexcept
    {
        CARLA_SAFE_ASSERT_RETURN(fIsActive,);

        fIsActive = false;
        fPlugin->setActive(false, false, false);
    }

    void lv2_run(const uint32_t frames)
    {
        //const PendingRtEventsRunner prt(this, frames);

        if (! lv2_pre_run(frames))
        {
            updateParameterOutputs();
            return;
        }

        if (fPorts.numMidiIns > 0)
        {
            uint32_t engineEventIndex = 0;
            carla_zeroStructs(pData->events.in, kMaxEngineEventInternalCount);

            for (uint32_t i=0; i < fPorts.numMidiIns; ++i)
            {
                LV2_ATOM_SEQUENCE_FOREACH(fPorts.eventsIn[i], event)
                {
                    if (event == nullptr)
                        continue;
                    if (event->body.type != fURIs.midiEvent)
                        continue;
                    if (event->body.size > 4)
                        continue;
                    if (event->time.frames >= frames)
                        break;

                    const uint8_t* const data((const uint8_t*)(event + 1));

                    EngineEvent& engineEvent(pData->events.in[engineEventIndex++]);

                    engineEvent.time = (uint32_t)event->time.frames;
                    engineEvent.fillFromMidiData((uint8_t)event->body.size, data, (uint8_t)i);

                    if (engineEventIndex >= kMaxEngineEventInternalCount)
                        break;
                }
            }
        }

        if (fPorts.numMidiOuts > 0)
        {
            carla_zeroStructs(pData->events.out, kMaxEngineEventInternalCount);
        }

        if (fPlugin->tryLock(fIsOffline))
        {
            fPlugin->initBuffers();
            fPlugin->process(fPorts.audioIns, fPorts.audioOuts, nullptr, nullptr, frames);
            fPlugin->unlock();

            if (fPorts.numMidiOuts > 0)
            {
                uint8_t        port    = 0;
                uint8_t        size    = 0;
                uint8_t        data[3] = { 0, 0, 0 };
                const uint8_t* dataPtr = data;

                for (ushort i=0; i < kMaxEngineEventInternalCount; ++i)
                {
                    const EngineEvent& engineEvent(pData->events.out[i]);

                    switch (engineEvent.type)
                    {
                    case kEngineEventTypeNull:
                        break;

                    case kEngineEventTypeControl: {
                        const EngineControlEvent& ctrlEvent(engineEvent.ctrl);
                        ctrlEvent.convertToMidiData(engineEvent.channel, size, data);
                        dataPtr = data;
                        break;
                    }

                    case kEngineEventTypeMidi: {
                        const EngineMidiEvent& midiEvent(engineEvent.midi);

                        port = midiEvent.port;
                        size = midiEvent.size;

                        if (size > EngineMidiEvent::kDataSize && midiEvent.dataExt != nullptr)
                            dataPtr = midiEvent.dataExt;
                        else
                            dataPtr = midiEvent.data;

                        break;
                    }
                    }

                    if (size > 0 && ! writeMidiEvent(port, engineEvent.time, size, dataPtr))
                        break;
                }
            }
        }
        else
        {
            for (uint32_t i=0; i<fPorts.numAudioOuts; ++i)
                carla_zeroFloats(fPorts.audioOuts[i], frames);
        }

        lv2_post_run(frames);
        updateParameterOutputs();

    }

    // ----------------------------------------------------------------------------------------------------------------

    bool lv2ui_instantiate(LV2UI_Write_Function writeFunction, LV2UI_Controller controller,
                           LV2UI_Widget* widget, const LV2_Feature* const* features)
    {
        fUI.writeFunction = writeFunction;
        fUI.controller = controller;
        fUI.host = nullptr;

        fUiName.clear();

        const LV2_URID_Map* uridMap = nullptr;

        // ------------------------------------------------------------------------------------------------------------
        // see if the host supports external-ui, get uridMap

        for (int i=0; features[i] != nullptr; ++i)
        {
            if (std::strcmp(features[i]->URI, LV2_EXTERNAL_UI__Host) == 0 ||
                std::strcmp(features[i]->URI, LV2_EXTERNAL_UI_DEPRECATED_URI) == 0)
            {
                fUI.host = (const LV2_External_UI_Host*)features[i]->data;
            }
            else if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
            {
                uridMap = (const LV2_URID_Map*)features[i]->data;
            }
        }

        if (fUI.host != nullptr)
        {
            fUiName = fUI.host->plugin_human_id;
            *widget = (LV2_External_UI_Widget_Compat*)this;
            return true;
        }

        // ------------------------------------------------------------------------------------------------------------
        // no external-ui support, use showInterface

        for (int i=0; features[i] != nullptr; ++i)
        {
            if (std::strcmp(features[i]->URI, LV2_OPTIONS__options) == 0)
            {
                const LV2_Options_Option* const options((const LV2_Options_Option*)features[i]->data);

                for (int j=0; options[j].key != 0; ++j)
                {
                    if (options[j].key == uridMap->map(uridMap->handle, LV2_UI__windowTitle))
                    {
                        fUiName = (const char*)options[j].value;
                        break;
                    }
                }
                break;
            }
        }

        if (fUiName.isEmpty())
            fUiName = fPlugin->getName();

        *widget = nullptr;
        return true;
    }

    void lv2ui_port_event(uint32_t portIndex, uint32_t bufferSize, uint32_t format, const void* buffer) const
    {
        if (format != 0 || bufferSize != sizeof(float) || buffer == nullptr)
            return;
        if (portIndex >= fPorts.indexOffset || ! fUI.isVisible)
            return;

        const float value(*(const float*)buffer);
        fPlugin->uiParameterChange(portIndex-fPorts.indexOffset, value);
    }

protected:
    // ----------------------------------------------------------------------------------------------------------------
    // CarlaEngine virtual calls

    bool init(const char* const clientName) override
    {
        carla_stdout("CarlaEngineNative::init(\"%s\")", clientName);

        if (! pData->init(clientName))
        {
            close();
            setLastError("Failed to init internal data");
            return false;
        }

        return true;
    }

    bool isRunning() const noexcept override
    {
        return fIsActive;
    }

    bool isOffline() const noexcept override
    {
        return fIsOffline;
    }

    bool usesConstantBufferSize() const noexcept override
    {
        return false;
    }

    EngineType getType() const noexcept override
    {
        return kEngineTypePlugin;
    }

    const char* getCurrentDriverName() const noexcept override
    {
        return "LV2 Plugin";
    }

    void engineCallback(const EngineCallbackOpcode action, const uint pluginId, const int value1, const int value2, const float value3, const char* const valueStr)
    {
        switch (action)
        {
        case ENGINE_CALLBACK_PARAMETER_VALUE_CHANGED:
            CARLA_SAFE_ASSERT_RETURN(value1 >= 0,);
            if (fUI.writeFunction != nullptr && fUI.controller != nullptr && fUI.isVisible)
            {
                fUI.writeFunction(fUI.controller,
                                  static_cast<uint32_t>(value1)+fPorts.indexOffset,
                                  sizeof(float), 0, &value3);
            }
            break;

        case ENGINE_CALLBACK_UI_STATE_CHANGED:
            fUI.isVisible = (value1 == 1);
            if (fUI.host != nullptr)
                fUI.host->ui_closed(fUI.controller);
            break;

        case ENGINE_CALLBACK_IDLE:
            break;

        default:
            carla_stdout("engineCallback(%i:%s, %u, %i, %i, %f, %s)",
                         action, EngineCallbackOpcode2Str(action), pluginId, value1, value2, value3, valueStr);
            break;
        }
    }

    // ----------------------------------------------------------------------------------------------------------------

    void handleUiRun() const override
    {
        try {
            fPlugin->uiIdle();
        } CARLA_SAFE_EXCEPTION("fPlugin->uiIdle()")
    }

    void handleUiShow() override
    {
        fPlugin->showCustomUI(true);
        fUI.isVisible = true;
    }

    void handleUiHide() override
    {
        fUI.isVisible = false;
        fPlugin->showCustomUI(false);
    }

    // ----------------------------------------------------------------------------------------------------------------

    void handleParameterValueChanged(const uint32_t index, const float value) override
    {
        fPlugin->setParameterValue(index, value, false, false, false);
    }

    void handleBufferSizeChanged(const uint32_t bufferSize) override
    {
        CarlaEngine::bufferSizeChanged(bufferSize);
    }

    void handleSampleRateChanged(const double sampleRate) override
    {
        CarlaEngine::sampleRateChanged(sampleRate);
    }

    // ----------------------------------------------------------------------------------------------------------------

private:
    CarlaPlugin* fPlugin;
    CarlaString fUiName;

    void updateParameterOutputs() noexcept
    {
        float value;

        for (uint32_t i=0; i < fPorts.numParams; ++i)
        {
            if (! fPorts.paramsOut[i])
                continue;

            fPorts.paramsLast[i] = value = fPlugin->getParameterValue(i);

            if (fPorts.paramsPtr[i] != nullptr)
                *fPorts.paramsPtr[i] = value;
        }
    }

    bool writeMidiEvent(const uint8_t port, const uint32_t time, const uint8_t midiSize, const uint8_t* midiData)
    {
        CARLA_SAFE_ASSERT_RETURN(fPorts.numMidiOuts > 0, false);
        CARLA_SAFE_ASSERT_RETURN(port < fPorts.numMidiOuts, false);
        CARLA_SAFE_ASSERT_RETURN(midiData != nullptr, false);
        CARLA_SAFE_ASSERT_RETURN(midiSize > 0, false);

        LV2_Atom_Sequence* const seq(fPorts.midiOuts[port]);
        CARLA_SAFE_ASSERT_RETURN(seq != nullptr, false);

        Ports::MidiOutData& mData(fPorts.midiOutData[port]);

        if (sizeof(LV2_Atom_Event) + midiSize > mData.capacity - mData.offset)
            return false;

        LV2_Atom_Event* const aev = (LV2_Atom_Event*)(LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq) + mData.offset);

        aev->time.frames = time;
        aev->body.size   = midiSize;
        aev->body.type   = fURIs.midiEvent;
        std::memcpy(LV2_ATOM_BODY(&aev->body), midiData, midiSize);

        const uint32_t size = lv2_atom_pad_size(static_cast<uint32_t>(sizeof(LV2_Atom_Event) + midiSize));
        mData.offset       += size;
        seq->atom.size     += size;

        return true;
    }

    // -------------------------------------------------------------------

    #define handlePtr ((CarlaEngineLV2Single*)handle)

    static void _engine_callback(void* handle, EngineCallbackOpcode action, uint pluginId, int value1, int value2, float value3, const char* valueStr)
    {
        handlePtr->engineCallback(action, pluginId, value1, value2, value3, valueStr);
    }

    #undef handlePtr

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaEngineLV2Single)
};

CARLA_BACKEND_END_NAMESPACE

using CarlaBackend::CarlaEngineLV2Single;

// --------------------------------------------------------------------------------------------------------------------
// LV2 DSP functions

static LV2_Handle lv2_instantiate(const LV2_Descriptor* lv2Descriptor, double sampleRate, const char* bundlePath, const LV2_Feature* const* features)
{
    carla_stdout("lv2_instantiate(%p, %g, %s, %p)", lv2Descriptor, sampleRate, bundlePath, features);

    CarlaEngineLV2Single* const instance(new CarlaEngineLV2Single(sampleRate, bundlePath, features));

    if (instance->hasPlugin())
        return (LV2_Handle)instance;

    delete instance;
    return nullptr;
}

#define instancePtr ((CarlaEngineLV2Single*)instance)

static void lv2_connect_port(LV2_Handle instance, uint32_t port, void* dataLocation)
{
    instancePtr->lv2_connect_port(port, dataLocation);
}

static void lv2_activate(LV2_Handle instance)
{
    carla_debug("lv2_activate(%p)", instance);
    instancePtr->lv2_activate();
}

static void lv2_run(LV2_Handle instance, uint32_t sampleCount)
{
    instancePtr->lv2_run(sampleCount);
}

static void lv2_deactivate(LV2_Handle instance)
{
    carla_debug("lv2_deactivate(%p)", instance);
    instancePtr->lv2_deactivate();
}

static void lv2_cleanup(LV2_Handle instance)
{
    carla_debug("lv2_cleanup(%p)", instance);
    delete instancePtr;
}

static const void* lv2_extension_data(const char* uri)
{
    carla_debug("lv2_extension_data(\"%s\")", uri);
    return nullptr;

    // unused
    (void)uri;
}

#undef instancePtr

// --------------------------------------------------------------------------------------------------------------------
// LV2 UI functions

static LV2UI_Handle lv2ui_instantiate(const LV2UI_Descriptor*, const char*, const char*,
                                      LV2UI_Write_Function writeFunction, LV2UI_Controller controller,
                                      LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    carla_debug("lv2ui_instantiate(..., %p, %p, %p)", writeFunction, controller, widget, features);

    CarlaEngineLV2Single* engine = nullptr;

    for (int i=0; features[i] != nullptr; ++i)
    {
        if (std::strcmp(features[i]->URI, LV2_INSTANCE_ACCESS_URI) == 0)
        {
            engine = (CarlaEngineLV2Single*)features[i]->data;
            break;
        }
    }

    if (engine == nullptr)
    {
        carla_stderr("Host doesn't support instance-access, cannot show UI");
        return nullptr;
    }

    if (! engine->lv2ui_instantiate(writeFunction, controller, widget, features))
        return nullptr;

    return (LV2UI_Handle)engine;
}

#define uiPtr ((CarlaEngineLV2Single*)ui)

static void lv2ui_port_event(LV2UI_Handle ui, uint32_t portIndex, uint32_t bufferSize, uint32_t format, const void* buffer)
{
    carla_debug("lv2ui_port_event(%p, %i, %i, %i, %p)", ui, portIndex, bufferSize, format, buffer);
    uiPtr->lv2ui_port_event(portIndex, bufferSize, format, buffer);
}

static void lv2ui_cleanup(LV2UI_Handle ui)
{
    carla_debug("lv2ui_cleanup(%p)", ui);
    uiPtr->lv2ui_cleanup();
}

static int lv2ui_idle(LV2UI_Handle ui)
{
    return uiPtr->lv2ui_idle();
}

static int lv2ui_show(LV2UI_Handle ui)
{
    carla_debug("lv2ui_show(%p)", ui);
    return uiPtr->lv2ui_show();
}

static int lv2ui_hide(LV2UI_Handle ui)
{
    carla_debug("lv2ui_hide(%p)", ui);
    return uiPtr->lv2ui_hide();
}

static const void* lv2ui_extension_data(const char* uri)
{
    carla_debug("lv2ui_extension_data(\"%s\")", uri);

    static const LV2UI_Idle_Interface uiidle = { lv2ui_idle };
    static const LV2UI_Show_Interface uishow = { lv2ui_show, lv2ui_hide };

    if (std::strcmp(uri, LV2_UI__idleInterface) == 0)
        return &uiidle;
    if (std::strcmp(uri, LV2_UI__showInterface) == 0)
        return &uishow;

    return nullptr;
}

#undef uiPtr

// --------------------------------------------------------------------------------------------------------------------
// Startup code

CARLA_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    carla_stdout("lv2_descriptor(%i)", index);

    if (index != 0)
        return nullptr;

    static CarlaString ret;

    if (ret.isEmpty())
    {
        using namespace water;
        const File file(File::getSpecialLocation(File::currentExecutableFile).withFileExtension("ttl"));
        ret = String("file://" + file.getFullPathName()).toRawUTF8();
    }

    static const LV2_Descriptor desc = {
    /* URI            */ ret.buffer(),
    /* instantiate    */ lv2_instantiate,
    /* connect_port   */ lv2_connect_port,
    /* activate       */ lv2_activate,
    /* run            */ lv2_run,
    /* deactivate     */ lv2_deactivate,
    /* cleanup        */ lv2_cleanup,
    /* extension_data */ lv2_extension_data
    };

    return &desc;
}

CARLA_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    carla_stdout("lv2ui_descriptor(%i)", index);

    static CarlaString ret;

    if (ret.isEmpty())
    {
        using namespace water;
        const File file(File::getSpecialLocation(File::currentExecutableFile).getSiblingFile("ext-ui"));
        ret = String("file://" + file.getFullPathName()).toRawUTF8();
    }

    static const LV2UI_Descriptor lv2UiExtDesc = {
    /* URI            */ ret.buffer(),
    /* instantiate    */ lv2ui_instantiate,
    /* cleanup        */ lv2ui_cleanup,
    /* port_event     */ lv2ui_port_event,
    /* extension_data */ lv2ui_extension_data
    };

    return (index == 0) ? &lv2UiExtDesc : nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
