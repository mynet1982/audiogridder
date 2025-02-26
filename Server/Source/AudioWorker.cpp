/*
 * Copyright (c) 2020 Andreas Pohl
 * Licensed under MIT (https://github.com/apohl79/audiogridder/blob/master/COPYING)
 *
 * Author: Andreas Pohl
 */

#include "AudioWorker.hpp"
#include <memory>
#include "Message.hpp"
#include "Defaults.hpp"
#include "App.hpp"
#include "Metrics.hpp"

namespace e47 {

std::unordered_map<String, AudioWorker::RecentsListType> AudioWorker::m_recents;
std::mutex AudioWorker::m_recentsMtx;

AudioWorker::AudioWorker(LogTag* tag) : Thread("AudioWorker"), LogTagDelegate(tag), m_channelMapper(tag) {
    initAsyncFunctors();
}

AudioWorker::~AudioWorker() {
    traceScope();
    stopAsyncFunctors();
    if (nullptr != m_socket && m_socket->isConnected()) {
        m_socket->close();
    }
    waitForThreadAndLog(getLogTagSource(), this);
}

void AudioWorker::init(std::unique_ptr<StreamingSocket> s, int channelsIn, int channelsOut, int channelsSC,
                       uint64 activeChannels, double rate, int samplesPerBlock, bool doublePrecission) {
    traceScope();
    m_socket = std::move(s);
    m_rate = rate;
    m_samplesPerBlock = samplesPerBlock;
    m_doublePrecission = doublePrecission;
    m_channelsIn = channelsIn;
    m_channelsOut = channelsOut;
    m_channelsSC = channelsSC;
    m_activeChannels = activeChannels;
    m_activeChannels.setWithInput(m_channelsIn > 0);
    m_activeChannels.setNumChannels(m_channelsIn + m_channelsSC, m_channelsOut);
    m_channelMapper.createMapping(m_activeChannels);
    m_channelMapper.print();
    m_chain =
        std::make_shared<ProcessorChain>(ProcessorChain::createBussesProperties(channelsIn, channelsOut, channelsSC));
    m_chain->setLogTagSource(getLogTagSource());
    if (m_doublePrecission && m_chain->supportsDoublePrecisionProcessing()) {
        m_chain->setProcessingPrecision(AudioProcessor::doublePrecision);
    }
    m_chain->updateChannels(channelsIn, channelsOut, channelsSC);
}

bool AudioWorker::waitForData() {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_socket->waitUntilReady(true, 50);
}

void AudioWorker::run() {
    traceScope();
    logln("audio processor started");

    AudioBuffer<float> bufferF;
    AudioBuffer<double> bufferD;
    MidiBuffer midi;
    AudioMessage msg(getLogTagSource());
    AudioPlayHead::CurrentPositionInfo posInfo;
    auto duration = TimeStatistic::getDuration("audio");
    auto bytesIn = Metrics::getStatistic<Meter>("NetBytesIn");
    auto bytesOut = Metrics::getStatistic<Meter>("NetBytesOut");

    ProcessorChain::PlayHead playHead(&posInfo);
    m_chain->prepareToPlay(m_rate, m_samplesPerBlock);
    bool hasToSetPlayHead = true;

    MessageHelper::Error e;
    while (isOk()) {
        // Read audio chunk
        if (waitForData()) {
            if (msg.readFromClient(m_socket.get(), bufferF, bufferD, midi, posInfo, &e, *bytesIn)) {
                std::lock_guard<std::mutex> lock(m_mtx);
                duration.reset();
                if (hasToSetPlayHead) {  // do not set the playhead before it's initialized
                    m_chain->setPlayHead(&playHead);
                    hasToSetPlayHead = false;
                }
                int bufferChannels = msg.isDouble() ? bufferD.getNumChannels() : bufferF.getNumChannels();
                int neededChannels = m_activeChannels.getNumActiveChannels(true);
                if (neededChannels > bufferChannels) {
                    logln("error processing audio message: buffer has not enough channels: needed channels is "
                          << neededChannels << ", but buffer has " << bufferChannels);
                    m_chain->releaseResources();
                    m_socket->close();
                    break;
                }
                bool sendOk;
                if (msg.isDouble()) {
                    if (m_chain->supportsDoublePrecisionProcessing()) {
                        processBlock(bufferD, midi);
                    } else {
                        bufferF.makeCopyOf(bufferD);
                        processBlock(bufferF, midi);
                        bufferD.makeCopyOf(bufferF);
                    }
                    sendOk = msg.sendToClient(m_socket.get(), bufferD, midi, m_chain->getLatencySamples(),
                                              bufferD.getNumChannels(), &e, *bytesOut);
                } else {
                    processBlock(bufferF, midi);
                    sendOk = msg.sendToClient(m_socket.get(), bufferF, midi, m_chain->getLatencySamples(),
                                              bufferF.getNumChannels(), &e, *bytesOut);
                }
                if (!sendOk) {
                    logln("error: failed to send audio data to client: " << e.toString());
                    m_socket->close();
                }
                duration.update();
            } else {
                logln("error: failed to read audio message: " << e.toString());
                m_socket->close();
            }
        }
    }

    m_chain->setPlayHead(nullptr);

    duration.clear();
    clear();
    signalThreadShouldExit();
    logln("audio processor terminated");
}

template <typename T>
void AudioWorker::processBlock(AudioBuffer<T>& buffer, MidiBuffer& midi) {
    int numChannels = jmax(m_channelsIn + m_channelsSC, m_channelsOut) + m_chain->getExtraChannels();
    if (numChannels <= buffer.getNumChannels()) {
        m_chain->processBlock(buffer, midi);
    } else {
        // we received less channels, now we need to map the input/output data
        auto* procBuffer = getProcBuffer<T>();
        procBuffer->setSize(numChannels, buffer.getNumSamples());
        if (m_activeChannels.getNumActiveChannels(true) > 0) {
            m_channelMapper.map(&buffer, procBuffer);
        } else {
            procBuffer->clear();
        }
        m_chain->processBlock(*procBuffer, midi);
        m_channelMapper.mapReverse(procBuffer, &buffer);
    }
}

void AudioWorker::shutdown() {
    traceScope();
    signalThreadShouldExit();
}

void AudioWorker::clear() {
    traceScope();
    if (nullptr != m_chain) {
        m_chain->clear();
    }
}

bool AudioWorker::addPlugin(const String& id, String& err) {
    traceScope();
    return m_chain->addPluginProcessor(id, err);
}

void AudioWorker::delPlugin(int idx) {
    traceScope();
    logln("deleting plugin " << idx);
    m_chain->delProcessor(idx);
}

void AudioWorker::exchangePlugins(int idxA, int idxB) {
    traceScope();
    logln("exchanging plugins idxA=" << idxA << " idxB=" << idxB);
    m_chain->exchangeProcessors(idxA, idxB);
}

String AudioWorker::getRecentsList(String host) const {
    traceScope();
    std::lock_guard<std::mutex> lock(m_recentsMtx);
    if (m_recents.find(host) == m_recents.end()) {
        return "";
    }
    auto& recents = m_recents[host];
    String list;
    for (auto& r : recents) {
        list += AGProcessor::createString(r) + "\n";
    }
    return list;
}

void AudioWorker::addToRecentsList(const String& id, const String& host) {
    traceScope();
    auto plug = AGProcessor::findPluginDescritpion(id);
    if (plug != nullptr) {
        std::lock_guard<std::mutex> lock(m_recentsMtx);
        auto& recents = m_recents[host];
        recents.removeAllInstancesOf(*plug);
        recents.insert(0, *plug);
        int toRemove = recents.size() - Defaults::DEFAULT_NUM_RECENTS;
        if (toRemove > 0) {
            recents.removeLast(toRemove);
        }
    }
}

}  // namespace e47
