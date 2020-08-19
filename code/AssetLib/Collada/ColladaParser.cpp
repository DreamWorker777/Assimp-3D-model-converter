/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2020, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

* Redistributions of source code must retain the above
copyright notice, this list of conditions and the
following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other
materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
contributors may be used to endorse or promote products
derived from this software without specific prior
written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

/** @file ColladaParser.cpp
 *  @brief Implementation of the Collada parser helper
 */

#ifndef ASSIMP_BUILD_NO_COLLADA_IMPORTER

#include "ColladaParser.h"
#include <assimp/ParsingUtils.h>
#include <assimp/StringUtils.h>
#include <assimp/TinyFormatter.h>
#include <assimp/ZipArchiveIOSystem.h>
#include <assimp/commonMetaData.h>
#include <assimp/fast_atof.h>
#include <assimp/light.h>
#include <stdarg.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <sstream>

#include <memory>

using namespace Assimp;
using namespace Assimp::Collada;
using namespace Assimp::Formatter;

// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
ColladaParser::ColladaParser(IOSystem *pIOHandler, const std::string &pFile) :
        mFileName(pFile),
        mXmlParser(),
        mDataLibrary(),
        mAccessorLibrary(),
        mMeshLibrary(),
        mNodeLibrary(),
        mImageLibrary(),
        mEffectLibrary(),
        mMaterialLibrary(),
        mLightLibrary(),
        mCameraLibrary(),
        mControllerLibrary(),
        mRootNode(nullptr),
        mAnims(),
        mUnitSize(1.0f),
        mUpDirection(UP_Y),
        mFormat(FV_1_5_n) {
    // validate io-handler instance
    if (nullptr == pIOHandler) {
        throw DeadlyImportError("IOSystem is nullptr.");
    }

    std::unique_ptr<IOStream> daefile;
    std::unique_ptr<ZipArchiveIOSystem> zip_archive;

    // Determine type
    std::string extension = BaseImporter::GetExtension(pFile);
    if (extension != "dae") {
        zip_archive.reset(new ZipArchiveIOSystem(pIOHandler, pFile));
    }

    if (zip_archive && zip_archive->isOpen()) {
        std::string dae_filename = ReadZaeManifest(*zip_archive);

        if (dae_filename.empty()) {
            ThrowException(std::string("Invalid ZAE"));
        }

        daefile.reset(zip_archive->Open(dae_filename.c_str()));
        if (daefile == nullptr) {
            ThrowException(std::string("Invalid ZAE manifest: '") + std::string(dae_filename) + std::string("' is missing"));
        }
    } else {
        // attempt to open the file directly
        daefile.reset(pIOHandler->Open(pFile));
        if (daefile.get() == nullptr) {
            throw DeadlyImportError("Failed to open file '" + pFile + "'.");
        }
    }
    pugi::xml_node *root = mXmlParser.parse(daefile.get());
    // generate a XML reader for it
    if (nullptr == root) {
        ThrowException("Unable to read file, malformed XML");
    }

    // start reading
    ReadContents(*root);

    // read embedded textures
    if (zip_archive && zip_archive->isOpen()) {
        ReadEmbeddedTextures(*zip_archive);
    }
}

// ------------------------------------------------------------------------------------------------
// Destructor, private as well
ColladaParser::~ColladaParser() {
    for (NodeLibrary::iterator it = mNodeLibrary.begin(); it != mNodeLibrary.end(); ++it)
        delete it->second;
    for (MeshLibrary::iterator it = mMeshLibrary.begin(); it != mMeshLibrary.end(); ++it)
        delete it->second;
}

// ------------------------------------------------------------------------------------------------
// Read a ZAE manifest and return the filename to attempt to open
std::string ColladaParser::ReadZaeManifest(ZipArchiveIOSystem &zip_archive) {
    // Open the manifest
    std::unique_ptr<IOStream> manifestfile(zip_archive.Open("manifest.xml"));
    if (manifestfile == nullptr) {
        // No manifest, hope there is only one .DAE inside
        std::vector<std::string> file_list;
        zip_archive.getFileListExtension(file_list, "dae");

        if (file_list.empty()) {
            return std::string();
        }

        return file_list.front();
    }
    XmlParser manifestParser;
    XmlNode *root = manifestParser.parse(manifestfile.get());
    if (nullptr == root) {
        return std::string();
    }

    const std::string &name = root->name();
    if (name != "dae_root") {
        root = manifestParser.findNode("dae_root");
        if (nullptr == root) {
            return std::string();
        }
        const char *filepath = root->value();
        aiString ai_str(filepath);
        UriDecodePath(ai_str);
        return std::string(ai_str.C_Str());
    }

    return std::string();
}

// ------------------------------------------------------------------------------------------------
// Convert a path read from a collada file to the usual representation
void ColladaParser::UriDecodePath(aiString &ss) {
    // TODO: collada spec, p 22. Handle URI correctly.
    // For the moment we're just stripping the file:// away to make it work.
    // Windows doesn't seem to be able to find stuff like
    // 'file://..\LWO\LWO2\MappingModes\earthSpherical.jpg'
    if (0 == strncmp(ss.data, "file://", 7)) {
        ss.length -= 7;
        memmove(ss.data, ss.data + 7, ss.length);
        ss.data[ss.length] = '\0';
    }

    // Maxon Cinema Collada Export writes "file:///C:\andsoon" with three slashes...
    // I need to filter it without destroying linux paths starting with "/somewhere"
#if defined(_MSC_VER)
    if (ss.data[0] == '/' && isalpha((unsigned char)ss.data[1]) && ss.data[2] == ':') {
#else
    if (ss.data[0] == '/' && isalpha(ss.data[1]) && ss.data[2] == ':') {
#endif
        --ss.length;
        ::memmove(ss.data, ss.data + 1, ss.length);
        ss.data[ss.length] = 0;
    }

    // find and convert all %xy special chars
    char *out = ss.data;
    for (const char *it = ss.data; it != ss.data + ss.length; /**/) {
        if (*it == '%' && (it + 3) < ss.data + ss.length) {
            // separate the number to avoid dragging in chars from behind into the parsing
            char mychar[3] = { it[1], it[2], 0 };
            size_t nbr = strtoul16(mychar);
            it += 3;
            *out++ = (char)(nbr & 0xFF);
        } else {
            *out++ = *it++;
        }
    }

    // adjust length and terminator of the shortened string
    *out = 0;
    ai_assert(out > ss.data);
    ss.length = static_cast<ai_uint32>(out - ss.data);
}

// ------------------------------------------------------------------------------------------------
// Read bool from text contents of current element
bool ColladaParser::ReadBoolFromTextContent() {
    const char *cur = GetTextContent();
    if (nullptr == cur) {
        return false;
    }
    return (!ASSIMP_strincmp(cur, "true", 4) || '0' != *cur);
}

// ------------------------------------------------------------------------------------------------
// Read float from text contents of current element
ai_real ColladaParser::ReadFloatFromTextContent() {
    const char *cur = GetTextContent();
    if (nullptr == cur) {
        return 0.0;
    }
    return fast_atof(cur);
}

// ------------------------------------------------------------------------------------------------
// Reads the contents of the file
void ColladaParser::ReadContents(XmlNode &node) {
    for (pugi::xml_node &curNode : node.children()) {
        pugi::xml_attribute attr = curNode.attribute("version");
        if (attr) {
            const char *version = attr.as_string();
            aiString v;
            v.Set(version);
            mAssetMetaData.emplace(AI_METADATA_SOURCE_FORMAT_VERSION, v);
            if (!::strncmp(version, "1.5", 3)) {
                mFormat = FV_1_5_n;
                ASSIMP_LOG_DEBUG("Collada schema version is 1.5.n");
            } else if (!::strncmp(version, "1.4", 3)) {
                mFormat = FV_1_4_n;
                ASSIMP_LOG_DEBUG("Collada schema version is 1.4.n");
            } else if (!::strncmp(version, "1.3", 3)) {
                mFormat = FV_1_3_n;
                ASSIMP_LOG_DEBUG("Collada schema version is 1.3.n");
            }
        }
        ReadStructure(curNode);
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the structure of the file
void ColladaParser::ReadStructure(XmlNode &node) {
    for (pugi::xml_node curNode : node.children()) {
        const std::string name = std::string(curNode.name());
        if (name == "asset")
            ReadAssetInfo(curNode);
        else if (name == "library_animations")
            ReadAnimationLibrary(curNode);
        else if (name == "library_animation_clips")
            ReadAnimationClipLibrary(curNode);
        else if (name == "library_controllers")
            ReadControllerLibrary(curNode);
        else if (name == "library_images")
            ReadImageLibrary(curNode);
        else if (name == "library_materials")
            ReadMaterialLibrary(curNode);
        else if (name == "library_effects")
            ReadEffectLibrary(curNode);
        else if (name == "library_geometries")
            ReadGeometryLibrary(curNode);
        else if (name == "library_visual_scenes")
            ReadSceneLibrary(curNode);
        else if (name == "library_lights")
            ReadLightLibrary(curNode);
        else if (name == "library_cameras")
            ReadCameraLibrary(curNode);
        else if (name == "library_nodes")
            ReadSceneNode(curNode, nullptr); /* some hacking to reuse this piece of code */
        else if (name == "scene")
            ReadScene(curNode);
    }

    PostProcessRootAnimations();
    PostProcessControllers();
}

// ------------------------------------------------------------------------------------------------
// Reads asset information such as coordinate system information and legal blah
void ColladaParser::ReadAssetInfo(XmlNode &node) {
    if (node.empty()) {
        return;
    }

    for (pugi::xml_node &curNode : node.children()) {
        const std::string name = std::string(curNode.name());
        if (name == "unit") {
            pugi::xml_attribute attr = curNode.attribute("meter");
            mUnitSize = 1.f;
            if (attr) {
                mUnitSize = static_cast<ai_real>(attr.as_double());
            }
        } else if (name == "up_axis") {
            const char *content = curNode.value();
            if (strncmp(content, "X_UP", 4) == 0) {
                mUpDirection = UP_X;
            } else if (strncmp(content, "Z_UP", 4) == 0) {
                mUpDirection = UP_Z;
            } else {
                mUpDirection = UP_Y;
            }
        } else if (name == "contributor") {
            ReadMetaDataItem(curNode, mAssetMetaData);
        }
    }
}

static bool FindCommonKey(const std::string &collada_key, const MetaKeyPairVector &key_renaming, size_t &found_index) {
    for (size_t i = 0; i < key_renaming.size(); ++i) {
        if (key_renaming[i].first == collada_key) {
            found_index = i;
            return true;
        }
    }
    found_index = std::numeric_limits<size_t>::max();

    return false;
}

// ------------------------------------------------------------------------------------------------
// Reads a single string metadata item
void ColladaParser::ReadMetaDataItem(XmlNode &node, StringMetaData &metadata) {
    const Collada::MetaKeyPairVector &key_renaming = GetColladaAssimpMetaKeysCamelCase();

    const std::string name = node.name();
    if (!name.empty()) {
        const char *value_char = node.value();
        if (nullptr != value_char) {
            aiString aistr;
            aistr.Set(value_char);

            std::string camel_key_str(name);
            ToCamelCase(camel_key_str);

            size_t found_index;
            if (FindCommonKey(camel_key_str, key_renaming, found_index)) {
                metadata.emplace(key_renaming[found_index].second, aistr);
            } else {
                metadata.emplace(camel_key_str, aistr);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the animation clips
void ColladaParser::ReadAnimationClipLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }

    std::string animName;
    pugi::xml_attribute nameAttr = node.attribute("name");
    if (nameAttr) {
        animName = nameAttr.as_string();
    } else {
        pugi::xml_attribute idAttr = node.attribute("id");
        if (idAttr) {
            animName = idAttr.as_string();
        } else {
            animName = std::string("animation_") + to_string(mAnimationClipLibrary.size());
        }
    }

    std::pair<std::string, std::vector<std::string>> clip;
    clip.first = animName;

    for (pugi::xml_node &curNode : node.children()) {
        const std::string currentName = curNode.name();
        if (currentName == "instance_animation") {
            pugi::xml_attribute url = curNode.attribute("url");
            if (url) {
                const std::string urlName = url.as_string();
                if (urlName[0] != '#') {
                    ThrowException("Unknown reference format");
                }
                clip.second.push_back(url.as_string());
            }
        }

        if (clip.second.size() > 0) {
            mAnimationClipLibrary.push_back(clip);
        }
    }
}

void ColladaParser::PostProcessControllers() {
    std::string meshId;
    for (ControllerLibrary::iterator it = mControllerLibrary.begin(); it != mControllerLibrary.end(); ++it) {
        meshId = it->second.mMeshId;
        ControllerLibrary::iterator findItr = mControllerLibrary.find(meshId);
        while (findItr != mControllerLibrary.end()) {
            meshId = findItr->second.mMeshId;
            findItr = mControllerLibrary.find(meshId);
        }

        it->second.mMeshId = meshId;
    }
}

// ------------------------------------------------------------------------------------------------
// Re-build animations from animation clip library, if present, otherwise combine single-channel animations
void ColladaParser::PostProcessRootAnimations() {
    if (mAnimationClipLibrary.empty()) {
        mAnims.CombineSingleChannelAnimations();
        return;
    }

    Animation temp;

    for (AnimationClipLibrary::iterator it = mAnimationClipLibrary.begin(); it != mAnimationClipLibrary.end(); ++it) {
        std::string clipName = it->first;

        Animation *clip = new Animation();
        clip->mName = clipName;

        temp.mSubAnims.push_back(clip);

        for (std::vector<std::string>::iterator a = it->second.begin(); a != it->second.end(); ++a) {
            std::string animationID = *a;

            AnimationLibrary::iterator animation = mAnimationLibrary.find(animationID);

            if (animation != mAnimationLibrary.end()) {
                Animation *pSourceAnimation = animation->second;

                pSourceAnimation->CollectChannelsRecursively(clip->mChannels);
            }
        }
    }

    mAnims = temp;

    // Ensure no double deletes.
    temp.mSubAnims.clear();
}

// ------------------------------------------------------------------------------------------------
// Reads the animation library
void ColladaParser::ReadAnimationLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }

    for (pugi::xml_node &curNode : node.children()) {
        const std::string currentName = curNode.name();
        if (currentName == "animation") {
            ReadAnimation(curNode, &mAnims);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an animation into the given parent structure
void ColladaParser::ReadAnimation(XmlNode &node, Collada::Animation *pParent) {
    if (node.empty()) {
        return;
    }

    // an <animation> element may be a container for grouping sub-elements or an animation channel
    // this is the channel collection by ID, in case it has channels
    typedef std::map<std::string, AnimationChannel> ChannelMap;
    ChannelMap channels;
    // this is the anim container in case we're a container
    Animation *anim = nullptr;

    // optional name given as an attribute
    std::string animName;
    pugi::xml_attribute nameAttr = node.attribute("name");
    if (nameAttr) {
        animName = nameAttr.as_string();
    } else {
        animName = "animation";
    }

    std::string animID;
    pugi::xml_attribute idAttr = node.attribute("id");
    if (idAttr) {
        animID = idAttr.as_string();
    }

    for (pugi::xml_node &curNode : node.children()) {
        const std::string currentName = curNode.name();
        if (currentName == "animation") {
            if (!anim) {
                anim = new Animation;
                anim->mName = animName;
                pParent->mSubAnims.push_back(anim);
            }

            // recurse into the sub-element
            ReadAnimation(curNode, anim);
        } else if (currentName == "source") {
            ReadSource(curNode);
        } else if (currentName == "sampler") {
            pugi::xml_attribute sampler_id = curNode.attribute("id");
            if (sampler_id) {
                std::string id = sampler_id.as_string();
                ChannelMap::iterator newChannel = channels.insert(std::make_pair(id, AnimationChannel())).first;

                // have it read into a channel
                ReadAnimationSampler(curNode, newChannel->second);
            } else if (currentName == "channel") {
                pugi::xml_attribute target = curNode.attribute("target");
                pugi::xml_attribute source = curNode.attribute("source");
                std::string source_name = source.as_string();
                if (source_name[0] == '#') {
                    source_name = source_name.substr(1, source_name.size() - 1);
                }
                ChannelMap::iterator cit = channels.find(source_name);
                if (cit != channels.end()) {
                    cit->second.mTarget = target.as_string();
                }
            }
        }
    }

    // it turned out to have channels - add them
    if (!channels.empty()) {
        // FIXME: Is this essentially doing the same as "single-anim-node" codepath in
        //        ColladaLoader::StoreAnimations? For now, this has been deferred to after
        //        all animations and all clips have been read. Due to handling of
        //        <library_animation_clips> this cannot be done here, as the channel owner
        //        is lost, and some exporters make up animations by referring to multiple
        //        single-channel animations from an <instance_animation>.
        /*
        // special filtering for stupid exporters packing each channel into a separate animation
        if( channels.size() == 1)
        {
            pParent->mChannels.push_back( channels.begin()->second);
        } else
*/
        {
            // else create the animation, if not done yet, and store the channels
            if (!anim) {
                anim = new Animation;
                anim->mName = animName;
                pParent->mSubAnims.push_back(anim);
            }
            for (ChannelMap::const_iterator it = channels.begin(); it != channels.end(); ++it)
                anim->mChannels.push_back(it->second);

            if (idAttr >= 0) {
                mAnimationLibrary[animID] = anim;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an animation sampler into the given anim channel
void ColladaParser::ReadAnimationSampler(XmlNode &node, Collada::AnimationChannel &pChannel) {
    for (pugi::xml_node &curNode : node.children()) {
        const std::string currentName = curNode.name();
        if (currentName == "input") {
            pugi::xml_attribute semanticAttr = curNode.attribute("semantic");
            if (!semanticAttr.empty()) {
                const char *semantic = semanticAttr.as_string();
                pugi::xml_attribute sourceAttr = curNode.attribute("source");
                if (!sourceAttr.empty()) {
                    const char *source = sourceAttr.as_string();
                    if (source[0] != '#')
                        ThrowException("Unsupported URL format");
                    source++;

                    if (strcmp(semantic, "INPUT") == 0)
                        pChannel.mSourceTimes = source;
                    else if (strcmp(semantic, "OUTPUT") == 0)
                        pChannel.mSourceValues = source;
                    else if (strcmp(semantic, "IN_TANGENT") == 0)
                        pChannel.mInTanValues = source;
                    else if (strcmp(semantic, "OUT_TANGENT") == 0)
                        pChannel.mOutTanValues = source;
                    else if (strcmp(semantic, "INTERPOLATION") == 0)
                        pChannel.mInterpolationValues = source;
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the skeleton controller library
void ColladaParser::ReadControllerLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    const std::string name = node.name();
    if (name != "controller") {
        return;
    }

    int attrId = node.attribute("id").as_int();
    std::string id = node.value();
    mControllerLibrary[id] = Controller();
    for (XmlNode currentNode : node.children()) {
        const std::string currentName = currentNode.name();
        if (currentName == "controller") {
            attrId = currentNode.attribute("id").as_int();
            std::string controllerId = currentNode.attribute(std::to_string(attrId).c_str()).value();
            ReadController(node, mControllerLibrary[controllerId]);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a controller into the given mesh structure
void ColladaParser::ReadController(XmlNode &node, Collada::Controller &pController) {
    // initial values
    pController.mType = Skin;
    pController.mMethod = Normalized;
    for (XmlNode currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "morph") {
            pController.mType = Morph;
            pController.mMeshId = currentNode.attribute("source").as_string();
            int methodIndex = currentNode.attribute("method").as_int();
            if (methodIndex > 0) {
                const char *method = currentNode.attribute("method").value();
                if (strcmp(method, "RELATIVE") == 0) {
                    pController.mMethod = Relative;
                }
            }
        } else if (currentName == "skin") {
            pController.mMeshId = currentNode.attribute("source").as_string();
        } else if (currentName == "bind_shape_matrix") {
            const char *content = currentNode.value();
            for (unsigned int a = 0; a < 16; a++) {
                // read a number
                content = fast_atoreal_move<ai_real>(content, pController.mBindShapeMatrix[a]);
                // skip whitespace after it
                SkipSpacesAndLineEnd(&content);
            }
        } else if (currentName == "source") {
            ReadSource(currentNode);
        } else if (IsElement("joints")) {
            ReadControllerJoints(currentNode, pController);
        } else if (IsElement("vertex_weights")) {
            ReadControllerWeights(currentNode, pController);
        } else if (IsElement("targets")) {
            for (XmlNode currendChildNode : currentNode.children()) {
                const std::string currentChildName = currendChildNode.name();
                if (currentChildName == "input") {
                    const char *semantics = currendChildNode.attribute("semantic").as_string();
                    const char *source = currendChildNode.attribute("source").as_string();
                    if (strcmp(semantics, "MORPH_TARGET") == 0) {
                        pController.mMorphTarget = source + 1;
                    } else if (strcmp(semantics, "MORPH_WEIGHT") == 0) {
                        pController.mMorphWeight = source + 1;
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the joint definitions for the given controller
void ColladaParser::ReadControllerJoints(XmlNode &node, Collada::Controller &pController) {
    for (XmlNode currentNode : node.children()) {
        const std::string currentName = currentNode.name();
        if (currentName == "input") {
            const char *attrSemantic = currentNode.attribute("semantic").as_string();
            const char *attrSource = currentNode.attribute("source").as_string();
            if (attrSource[0] != '#') {
                ThrowException(format() << "Unsupported URL format in \"" << attrSource << "\" in source attribute of <joints> data <input> element");
            }
            ++attrSource;
            // parse source URL to corresponding source
            if (strcmp(attrSemantic, "JOINT") == 0) {
                pController.mJointNameSource = attrSource;
            } else if (strcmp(attrSemantic, "INV_BIND_MATRIX") == 0) {
                pController.mJointOffsetMatrixSource = attrSource;
            } else {
                ThrowException(format() << "Unknown semantic \"" << attrSemantic << "\" in <joints> data <input> element");
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the joint weights for the given controller
void ColladaParser::ReadControllerWeights(XmlNode &node, Collada::Controller &pController) {
    // Read vertex count from attributes and resize the array accordingly
    int vertexCount = node.attribute("count").as_int();
    pController.mWeightCounts.resize(vertexCount);

    /*// read vertex count from attributes and resize the array accordingly
    int indexCount = GetAttribute("count");
    size_t vertexCount = (size_t)mReader->getAttributeValueAsInt(indexCount);
    pController.mWeightCounts.resize(vertexCount);*/

    for (XmlNode currentNode : node.children()) {
        std::string currentName = currentNode.name();
        if (currentName == "input") {
            InputChannel channel;

            const char *attrSemantic = currentNode.attribute("semantic").as_string();
            const char *attrSource = currentNode.attribute("source").as_string();
            channel.mOffset = currentNode.attribute("offset").as_int();

            // local URLS always start with a '#'. We don't support global URLs
            if (attrSource[0] != '#') {
                ThrowException(format() << "Unsupported URL format in \"" << attrSource << "\" in source attribute of <vertex_weights> data <input> element");
            }
            channel.mAccessor = attrSource + 1;

            // parse source URL to corresponding source
            if (strcmp(attrSemantic, "JOINT") == 0) {
                pController.mWeightInputJoints = channel;
            } else if (strcmp(attrSemantic, "WEIGHT") == 0) {
                pController.mWeightInputWeights = channel;
            } else {
                ThrowException(format() << "Unknown semantic \"" << attrSemantic << "\" in <vertex_weights> data <input> element");
            }
        } else if (currentName == "vcount" && vertexCount > 0) {
            const char *text = currentNode.value();
            size_t numWeights = 0;
            for (std::vector<size_t>::iterator it = pController.mWeightCounts.begin(); it != pController.mWeightCounts.end(); ++it) {
                if (*text == 0) {
                    ThrowException("Out of data while reading <vcount>");
                }

                *it = strtoul10(text, &text);
                numWeights += *it;
                SkipSpacesAndLineEnd(&text);
            }
            // reserve weight count
            pController.mWeights.resize(numWeights);
        } else if (currentName == "v" && vertexCount > 0) {
            // read JointIndex - WeightIndex pairs
            const char *text = currentNode.value();
            for (std::vector<std::pair<size_t, size_t>>::iterator it = pController.mWeights.begin(); it != pController.mWeights.end(); ++it) {
                if (*text == 0) {
                    ThrowException("Out of data while reading <vertex_weights>");
                }
                it->first = strtoul10(text, &text);
                SkipSpacesAndLineEnd(&text);
                if (*text == 0)
                    ThrowException("Out of data while reading <vertex_weights>");
                it->second = strtoul10(text, &text);
                SkipSpacesAndLineEnd(&text);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the image library contents
void ColladaParser::ReadImageLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    for (XmlNode currentNode : node.children()) {
        const std::string name = currentNode.name();
        if (name == "image") {
            std::string id = currentNode.attribute("id").as_string();
            mImageLibrary[id] = Image();

            // read on from there
            ReadImage(currentNode, mImageLibrary[id]);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an image entry into the given image
void ColladaParser::ReadImage(XmlNode &node, Collada::Image &pImage) {
    for (XmlNode currentNode : node.children()) {
        const std::string currentName = currentNode.name();
        if (currentName == "image") {
            // Ignore
            continue;
        } else if (currentName == "init_from") {
            if (mFormat == FV_1_4_n) {
                // FIX: C4D exporter writes empty <init_from/> tags
                if (!currentNode.empty()) {
                    // element content is filename - hopefully
                    const char *sz = TestTextContent();
                    if (sz) {
                        aiString filepath(sz);
                        UriDecodePath(filepath);
                        pImage.mFileName = filepath.C_Str();
                    }
                    //                    TestClosing("init_from");
                }
                if (!pImage.mFileName.length()) {
                    pImage.mFileName = "unknown_texture";
                }
            } else if (mFormat == FV_1_5_n) {
                // make sure we skip over mip and array initializations, which
                // we don't support, but which could confuse the loader if
                // they're not skipped.
                int v = currentNode.attribute("ref").as_int();
/*                if (v y) {
                    ASSIMP_LOG_WARN("Collada: Ignoring texture array index");
                    continue;
                }*/

                v  = currentNode.attribute("mip_index").as_int();
                /*if (attrib != -1 && v > 0) {
                    ASSIMP_LOG_WARN("Collada: Ignoring MIP map layer");
                    continue;
                }*/

                // TODO: correctly jump over cube and volume maps?
            }
        } else if (mFormat == FV_1_5_n) {
            XmlNode refChild = currentNode.child("ref");
            XmlNode hexChild = currentNode.child("hex");
            if (refChild) {
                // element content is filename - hopefully
                const char *sz = refChild.value();
                if (sz) {
                    aiString filepath(sz);
                    UriDecodePath(filepath);
                    pImage.mFileName = filepath.C_Str();
                }
            } else if (hexChild && !pImage.mFileName.length()) {
                // embedded image. get format
                pImage.mEmbeddedFormat = hexChild.attribute("format").as_string();
                if (pImage.mEmbeddedFormat.empty()) {
                    ASSIMP_LOG_WARN("Collada: Unknown image file format");
                }

                const char *data = hexChild.value();

                // hexadecimal-encoded binary octets. First of all, find the
                // required buffer size to reserve enough storage.
                const char *cur = data;
                while (!IsSpaceOrNewLine(*cur)) {
                    ++cur;
                }

                const unsigned int size = (unsigned int)(cur - data) * 2;
                pImage.mImageData.resize(size);
                for (unsigned int i = 0; i < size; ++i) {
                    pImage.mImageData[i] = HexOctetToDecimal(data + (i << 1));
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the material library
void ColladaParser::ReadMaterialLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    std::map<std::string, int> names;

    for (XmlNode currentNode : node.children()) {
        const std::string currentName = currentNode.name();
        std::string id = currentNode.attribute("id").as_string();
        std::string name = currentNode.attribute("name").as_string();
        mMaterialLibrary[id] = Material();

        if (!name.empty()) {
            std::map<std::string, int>::iterator it = names.find(name);
            if (it != names.end()) {
                std::ostringstream strStream;
                strStream << ++it->second;
                name.append(" " + strStream.str());
            } else {
                names[name] = 0;
            }

            mMaterialLibrary[id].mName = name;
        }

        ReadMaterial(currentNode, mMaterialLibrary[id]);
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the light library
void ColladaParser::ReadLightLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }

    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "light") {
            std::string id = currentNode.attribute("id").as_string();
            ReadLight(currentNode, mLightLibrary[id] = Light());
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the camera library
void ColladaParser::ReadCameraLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "camera") {
            std::string id = currentNode.attribute("id").as_string();

            // create an entry and store it in the library under its ID
            Camera &cam = mCameraLibrary[id];
            std::string name = currentNode.attribute("name").as_string();
            if (!name.empty()) {
                cam.mName = name;
            }
            ReadCamera(currentNode, cam);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a material entry into the given material
void ColladaParser::ReadMaterial(XmlNode &node, Collada::Material &pMaterial) {
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "material") {
            const char *url = currentNode.attribute("url").as_string();
            //const char *url = mReader->getAttributeValue(attrUrl);
            if (url[0] != '#') {
                ThrowException("Unknown reference format");
            }
            pMaterial.mEffect = url + 1;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a light entry into the given light
void ColladaParser::ReadLight(XmlNode &node, Collada::Light &pLight) {
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "spot") {
            pLight.mType = aiLightSource_SPOT;
        } else if (currentName == "ambient") {
            pLight.mType = aiLightSource_AMBIENT;
        } else if (currentName == "directional") {
            pLight.mType = aiLightSource_DIRECTIONAL;
        } else if (currentName == "point") {
            pLight.mType = aiLightSource_POINT;
        } else if (currentName == "color") {
            // text content contains 3 floats
            const char *content = GetTextContent();

            content = fast_atoreal_move<ai_real>(content, (ai_real &)pLight.mColor.r);
            SkipSpacesAndLineEnd(&content);

            content = fast_atoreal_move<ai_real>(content, (ai_real &)pLight.mColor.g);
            SkipSpacesAndLineEnd(&content);

            content = fast_atoreal_move<ai_real>(content, (ai_real &)pLight.mColor.b);
            SkipSpacesAndLineEnd(&content);
        } else if (currentName == "constant_attenuation") {
            pLight.mAttConstant = ReadFloatFromTextContent();
        } else if (currentName == "linear_attenuation") {
            pLight.mAttLinear = ReadFloatFromTextContent();
        } else if (currentName == "quadratic_attenuation") {
            pLight.mAttQuadratic = ReadFloatFromTextContent();
        } else if (currentName == "falloff_angle") {
            pLight.mFalloffAngle = ReadFloatFromTextContent();
        } else if (currentName == "falloff_exponent") {
            pLight.mFalloffExponent = ReadFloatFromTextContent();
        }
        // FCOLLADA extensions
        // -------------------------------------------------------
        else if (currentName == "outer_cone") {
            pLight.mOuterAngle = ReadFloatFromTextContent();
        }
        // ... and this one is even deprecated
        else if (currentName == "penumbra_angle") {
            pLight.mPenumbraAngle = ReadFloatFromTextContent();
        } else if (currentName == "intensity") {
            pLight.mIntensity = ReadFloatFromTextContent();
        } else if (currentName == "falloff") {
            pLight.mOuterAngle = ReadFloatFromTextContent();
        } else if (currentName == "hotspot_beam") {
            pLight.mFalloffAngle = ReadFloatFromTextContent();
        }
        // OpenCOLLADA extensions
        // -------------------------------------------------------
        else if (currentName == "decay_falloff") {
            pLight.mOuterAngle = ReadFloatFromTextContent();
        }
    }

}

// ------------------------------------------------------------------------------------------------
// Reads a camera entry into the given light
void ColladaParser::ReadCamera(XmlNode &node, Collada::Camera &camera) {
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "orthographic") {
            camera.mOrtho = true;
        } else if (currentName == "xfov" || currentName == "xmag") {
            camera.mHorFov = ReadFloatFromTextContent();
        } else if (currentName == "yfov" || currentName == "ymag") {
            camera.mVerFov = ReadFloatFromTextContent();
        } else if (currentName == "aspect_ratio") {
            camera.mAspect = ReadFloatFromTextContent();
        } else if (currentName == "znear") {
            camera.mZNear = ReadFloatFromTextContent();
        } else if (currentName == "zfar") {
            camera.mZFar = ReadFloatFromTextContent();
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the effect library
void ColladaParser::ReadEffectLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement()) {
        return;
    }*/

    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "effect") {
            // read ID. Do I have to repeat my ranting about "optional" attributes?
            //int attrID = GetAttribute("id");
            std::string id = currentNode.attribute("id").as_string();

            // create an entry and store it in the library under its ID
            mEffectLibrary[id] = Effect();

            // read on from there
            ReadEffect(currentNode, mEffectLibrary[id]);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an effect entry into the given effect
void ColladaParser::ReadEffect(XmlNode &node, Collada::Effect &pEffect) {
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "profile_COMMON") {
            ReadEffectProfileCommon(currentNode, pEffect);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an COMMON effect profile
void ColladaParser::ReadEffectProfileCommon(XmlNode &node, Collada::Effect &pEffect) {
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        if (currentName == "newparam") {
            // save ID
            std::string sid = currentNode.attribute("sid").as_string();
            //std::string sid = GetAttribute("sid");
             //= mReader->getAttributeValue(attrSID);
            pEffect.mParams[sid] = EffectParam();
            ReadEffectParam(currentNode, pEffect.mParams[sid]);
        } else if (currentName == "technique" || currentName == "extra" ) {
            // just syntactic sugar
        } else if (mFormat == FV_1_4_n && currentName == "image") {
            // read ID. Another entry which is "optional" by design but obligatory in reality
            std::string id = currentNode.attribute("id").as_string();

            //int attrID = GetAttribute("id");
            //std::string id = mReader->getAttributeValue(attrID);

            // create an entry and store it in the library under its ID
            mImageLibrary[id] = Image();

            // read on from there
            ReadImage(currentNode, mImageLibrary[id]);
        } else if (currentName == "phong")
            pEffect.mShadeType = Shade_Phong;
        else if (currentName == "constant")
            pEffect.mShadeType = Shade_Constant;
        else if (currentName == "lambert")
            pEffect.mShadeType = Shade_Lambert;
        else if (currentName == "blinn")
            pEffect.mShadeType = Shade_Blinn;

        /* Color + texture properties */
        else if (currentName == "emission")
            ReadEffectColor(currentNode, pEffect.mEmissive, pEffect.mTexEmissive);
        else if (currentName == "ambient")
            ReadEffectColor(currentNode, pEffect.mAmbient, pEffect.mTexAmbient);
        else if (currentName == "diffuse")
            ReadEffectColor(currentNode, pEffect.mDiffuse, pEffect.mTexDiffuse);
        else if (currentName == "specular")
            ReadEffectColor(currentNode, pEffect.mSpecular, pEffect.mTexSpecular);
        else if (currentName == "reflective") {
            ReadEffectColor(currentNode, pEffect.mReflective, pEffect.mTexReflective);
        } else if (currentName == "transparent") {
            pEffect.mHasTransparency = true;
            const char *opaque = currentNode.attribute("opaque").as_string();
            //const char *opaque = mReader->getAttributeValueSafe("opaque");

            if (::strcmp(opaque, "RGB_ZERO") == 0 || ::strcmp(opaque, "RGB_ONE") == 0) {
                pEffect.mRGBTransparency = true;
            }

            // In RGB_ZERO mode, the transparency is interpreted in reverse, go figure...
            if (::strcmp(opaque, "RGB_ZERO") == 0 || ::strcmp(opaque, "A_ZERO") == 0) {
                pEffect.mInvertTransparency = true;
            }

            ReadEffectColor(currentNode, pEffect.mTransparent, pEffect.mTexTransparent);
        } else if (currentName == "shininess")
            ReadEffectFloat(currentNode, pEffect.mShininess);
        else if (currentName == "reflectivity")
            ReadEffectFloat(currentNode, pEffect.mReflectivity);

        /* Single scalar properties */
        else if (currentName == "transparency")
            ReadEffectFloat(currentNode, pEffect.mTransparency);
        else if (currentName == "index_of_refraction")
            ReadEffectFloat(currentNode, pEffect.mRefractIndex);

        // GOOGLEEARTH/OKINO extensions
        // -------------------------------------------------------
        else if (currentName == "double_sided")
            pEffect.mDoubleSided = ReadBoolFromTextContent();

        // FCOLLADA extensions
        // -------------------------------------------------------
        else if (currentName == "bump") {
            aiColor4D dummy;
            ReadEffectColor(currentNode, dummy, pEffect.mTexBump);
        }

        // MAX3D extensions
        // -------------------------------------------------------
        else if (currentName == "wireframe") {
            pEffect.mWireframe = ReadBoolFromTextContent();
        } else if (currentName == "faceted") {
            pEffect.mFaceted = ReadBoolFromTextContent();
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Read texture wrapping + UV transform settings from a profile==Maya chunk
void ColladaParser::ReadSamplerProperties(XmlNode &node, Sampler &out) {
    if (node.empty()) {
        return;
    }
    for (XmlNode &currentNode : node.children()) {
        const std::string &currentName = currentNode.name();
        // MAYA extensions
        // -------------------------------------------------------
        if (currentName == "wrapU") {
            out.mWrapU = ReadBoolFromTextContent();
        } else if (currentName == "wrapV") {
            out.mWrapV = ReadBoolFromTextContent();
        } else if (currentName == "mirrorU") {
            out.mMirrorU = ReadBoolFromTextContent();
        } else if (currentName == "mirrorV") {
            out.mMirrorV = ReadBoolFromTextContent();
        } else if (currentName  == "repeatU") {
            out.mTransform.mScaling.x = ReadFloatFromTextContent();
        } else if (currentName == "repeatV") {
            out.mTransform.mScaling.y = ReadFloatFromTextContent();
        } else if (currentName  == "offsetU") {
            out.mTransform.mTranslation.x = ReadFloatFromTextContent();
        } else if (currentName  == "offsetV") {
            out.mTransform.mTranslation.y = ReadFloatFromTextContent();
        } else if (currentName  == "rotateUV") {
            out.mTransform.mRotation = ReadFloatFromTextContent();
        } else if (currentName == "blend_mode") {

            const char *sz = GetTextContent();
            // http://www.feelingsoftware.com/content/view/55/72/lang,en/
            // NONE, OVER, IN, OUT, ADD, SUBTRACT, MULTIPLY, DIFFERENCE, LIGHTEN, DARKEN, SATURATE, DESATURATE and ILLUMINATE
            if (0 == ASSIMP_strincmp(sz, "ADD", 3))
                out.mOp = aiTextureOp_Add;
            else if (0 == ASSIMP_strincmp(sz, "SUBTRACT", 8))
                out.mOp = aiTextureOp_Subtract;
            else if (0 == ASSIMP_strincmp(sz, "MULTIPLY", 8))
                out.mOp = aiTextureOp_Multiply;
            else {
                ASSIMP_LOG_WARN("Collada: Unsupported MAYA texture blend mode");
            }
        }
        // OKINO extensions
        // -------------------------------------------------------
        else if (currentName == "weighting") {
            out.mWeighting = ReadFloatFromTextContent();
        } else if (currentName  == "mix_with_previous_layer") {
            out.mMixWithPrevious = ReadFloatFromTextContent();
        }
        // MAX3D extensions
        // -------------------------------------------------------
        else if (currentName  == "amount") {
            out.mWeighting = ReadFloatFromTextContent();
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an effect entry containing a color or a texture defining that color
void ColladaParser::ReadEffectColor(XmlNode &node, aiColor4D &pColor, Sampler &pSampler) {
    if (node.empty()) {
        return;
    }

    // Save current element name
    const std::string curElem = mReader->getNodeName();

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("color")) {
                // text content contains 4 floats
                const char *content = GetTextContent();

                content = fast_atoreal_move<ai_real>(content, (ai_real &)pColor.r);
                SkipSpacesAndLineEnd(&content);

                content = fast_atoreal_move<ai_real>(content, (ai_real &)pColor.g);
                SkipSpacesAndLineEnd(&content);

                content = fast_atoreal_move<ai_real>(content, (ai_real &)pColor.b);
                SkipSpacesAndLineEnd(&content);

                content = fast_atoreal_move<ai_real>(content, (ai_real &)pColor.a);
                SkipSpacesAndLineEnd(&content);
                TestClosing("color");
            } else if (IsElement("texture")) {
                // get name of source texture/sampler
                int attrTex = GetAttribute("texture");
                pSampler.mName = mReader->getAttributeValue(attrTex);

                // get name of UV source channel. Specification demands it to be there, but some exporters
                // don't write it. It will be the default UV channel in case it's missing.
                attrTex = TestAttribute("texcoord");
                if (attrTex >= 0)
                    pSampler.mUVChannel = mReader->getAttributeValue(attrTex);
                //SkipElement();

                // as we've read texture, the color needs to be 1,1,1,1
                pColor = aiColor4D(1.f, 1.f, 1.f, 1.f);
            } else if (IsElement("technique")) {
                const int _profile = GetAttribute("profile");
                const char *profile = mReader->getAttributeValue(_profile);

                // Some extensions are quite useful ... ReadSamplerProperties processes
                // several extensions in MAYA, OKINO and MAX3D profiles.
                if (!::strcmp(profile, "MAYA") || !::strcmp(profile, "MAX3D") || !::strcmp(profile, "OKINO")) {
                    // get more information on this sampler
                    ReadSamplerProperties(pSampler);
                } else
                    SkipElement();
            } else if (!IsElement("extra")) {
                // ignore the rest
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (mReader->getNodeName() == curElem)
                break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an effect entry containing a float
void ColladaParser::ReadEffectFloat(XmlNode &node, ai_real &pFloat) {
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("float")) {
                // text content contains a single floats
                const char *content = GetTextContent();
                content = fast_atoreal_move<ai_real>(content, pFloat);
                SkipSpacesAndLineEnd(&content);

                TestClosing("float");
            } else {
                // ignore the rest
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads an effect parameter specification of any kind
void ColladaParser::ReadEffectParam(XmlNode &node, Collada::EffectParam &pParam) {
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("surface")) {
                // image ID given inside <init_from> tags
                TestOpening("init_from");
                const char *content = GetTextContent();
                pParam.mType = Param_Surface;
                pParam.mReference = content;
                TestClosing("init_from");

                // don't care for remaining stuff
                SkipElement("surface");
            } else if (IsElement("sampler2D") && (FV_1_4_n == mFormat || FV_1_3_n == mFormat)) {
                // surface ID is given inside <source> tags
                TestOpening("source");
                const char *content = GetTextContent();
                pParam.mType = Param_Sampler;
                pParam.mReference = content;
                TestClosing("source");

                // don't care for remaining stuff
                SkipElement("sampler2D");
            } else if (IsElement("sampler2D")) {
                // surface ID is given inside <instance_image> tags
                TestOpening("instance_image");
                int attrURL = GetAttribute("url");
                const char *url = mReader->getAttributeValue(attrURL);
                if (url[0] != '#')
                    ThrowException("Unsupported URL format in instance_image");
                url++;
                pParam.mType = Param_Sampler;
                pParam.mReference = url;
                SkipElement("sampler2D");
            } else {
                // ignore unknown element
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the geometry library contents
void ColladaParser::ReadGeometryLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("geometry")) {
                // read ID. Another entry which is "optional" by design but obligatory in reality
                int indexID = GetAttribute("id");
                std::string id = mReader->getAttributeValue(indexID);

                // create a mesh and store it in the library under its (resolved) ID
                // Skip and warn if ID is not unique
                if (mMeshLibrary.find(id) == mMeshLibrary.cend()) {
                    std::unique_ptr<Mesh> mesh(new Mesh(id));

                    // read the mesh name if it exists
                    const int nameIndex = TestAttribute("name");
                    if (nameIndex != -1) {
                        mesh->mName = mReader->getAttributeValue(nameIndex);
                    }

                    // read on from there
                    ReadGeometry(*mesh);
                    // Read successfully, add to library
                    mMeshLibrary.insert({ id, mesh.release() });
                } else {
                    ASSIMP_LOG_ERROR_F("Collada: Skipped duplicate geometry id: \"", id, "\"");
                    SkipElement();
                }
            } else {
                // ignore the rest
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "library_geometries") != 0)
                ThrowException("Expected end of <library_geometries> element.");

            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a geometry from the geometry library.
void ColladaParser::ReadGeometry(XmlNode &node, Collada::Mesh &pMesh) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("mesh")) {
                // read on from there
                ReadMesh(pMesh);
            } else {
                // ignore the rest
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "geometry") != 0)
                ThrowException("Expected end of <geometry> element.");

            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a mesh from the geometry library
void ColladaParser::ReadMesh(XmlNode &node, Mesh &pMesh) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("source")) {
                // we have professionals dealing with this
                ReadSource();
            } else if (IsElement("vertices")) {
                // read per-vertex mesh data
                ReadVertexData(pMesh);
            } else if (IsElement("triangles") || IsElement("lines") || IsElement("linestrips") || IsElement("polygons") || IsElement("polylist") || IsElement("trifans") || IsElement("tristrips")) {
                // read per-index mesh data and faces setup
                ReadIndexData(pMesh);
            } else {
                // ignore the restf
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "technique_common") == 0) {
                // end of another meaningless element - read over it
            } else if (strcmp(mReader->getNodeName(), "mesh") == 0) {
                // end of <mesh> element - we're done here
                break;
            } else {
                // everything else should be punished
                ThrowException("Expected end of <mesh> element.");
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a source element
void ColladaParser::ReadSource(XmlNode &node) {
    int indexID = GetAttribute("id");
    std::string sourceID = mReader->getAttributeValue(indexID);

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("float_array") || IsElement("IDREF_array") || IsElement("Name_array")) {
                ReadDataArray();
            } else if (IsElement("technique_common")) {
                // I don't care for your profiles
            } else if (IsElement("accessor")) {
                ReadAccessor(sourceID);
            } else {
                // ignore the rest
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "source") == 0) {
                // end of <source> - we're done
                break;
            } else if (strcmp(mReader->getNodeName(), "technique_common") == 0) {
                // end of another meaningless element - read over it
            } else {
                // everything else should be punished
                ThrowException("Expected end of <source> element.");
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a data array holding a number of floats, and stores it in the global library
void ColladaParser::ReadDataArray(XmlNode &node) {
    std::string elmName = mReader->getNodeName();
    bool isStringArray = (elmName == "IDREF_array" || elmName == "Name_array");
    bool isEmptyElement = mReader->isEmptyElement();

    // read attributes
    int indexID = GetAttribute("id");
    std::string id = mReader->getAttributeValue(indexID);
    int indexCount = GetAttribute("count");
    unsigned int count = (unsigned int)mReader->getAttributeValueAsInt(indexCount);
    const char *content = TestTextContent();

    // read values and store inside an array in the data library
    mDataLibrary[id] = Data();
    Data &data = mDataLibrary[id];
    data.mIsStringArray = isStringArray;

    // some exporters write empty data arrays, but we need to conserve them anyways because others might reference them
    if (content) {
        if (isStringArray) {
            data.mStrings.reserve(count);
            std::string s;

            for (unsigned int a = 0; a < count; a++) {
                if (*content == 0)
                    ThrowException("Expected more values while reading IDREF_array contents.");

                s.clear();
                while (!IsSpaceOrNewLine(*content))
                    s += *content++;
                data.mStrings.push_back(s);

                SkipSpacesAndLineEnd(&content);
            }
        } else {
            data.mValues.reserve(count);

            for (unsigned int a = 0; a < count; a++) {
                if (*content == 0)
                    ThrowException("Expected more values while reading float_array contents.");

                ai_real value;
                // read a number
                content = fast_atoreal_move<ai_real>(content, value);
                data.mValues.push_back(value);
                // skip whitespace after it
                SkipSpacesAndLineEnd(&content);
            }
        }
    }

    // test for closing tag
    if (!isEmptyElement)
        TestClosing(elmName.c_str());
}

// ------------------------------------------------------------------------------------------------
// Reads an accessor and stores it in the global library
void ColladaParser::ReadAccessor(XmlNode &node, const std::string &pID) {
    // read accessor attributes
    int attrSource = GetAttribute("source");
    const char *source = mReader->getAttributeValue(attrSource);
    if (source[0] != '#')
        ThrowException(format() << "Unknown reference format in url \"" << source << "\" in source attribute of <accessor> element.");
    int attrCount = GetAttribute("count");
    unsigned int count = (unsigned int)mReader->getAttributeValueAsInt(attrCount);
    int attrOffset = TestAttribute("offset");
    unsigned int offset = 0;
    if (attrOffset > -1)
        offset = (unsigned int)mReader->getAttributeValueAsInt(attrOffset);
    int attrStride = TestAttribute("stride");
    unsigned int stride = 1;
    if (attrStride > -1)
        stride = (unsigned int)mReader->getAttributeValueAsInt(attrStride);

    // store in the library under the given ID
    mAccessorLibrary[pID] = Accessor();
    Accessor &acc = mAccessorLibrary[pID];
    acc.mCount = count;
    acc.mOffset = offset;
    acc.mStride = stride;
    acc.mSource = source + 1; // ignore the leading '#'
    acc.mSize = 0; // gets incremented with every param

    // and read the components
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("param")) {
                // read data param
                int attrName = TestAttribute("name");
                std::string name;
                if (attrName > -1) {
                    name = mReader->getAttributeValue(attrName);

                    // analyse for common type components and store it's sub-offset in the corresponding field

                    /* Cartesian coordinates */
                    if (name == "X")
                        acc.mSubOffset[0] = acc.mParams.size();
                    else if (name == "Y")
                        acc.mSubOffset[1] = acc.mParams.size();
                    else if (name == "Z")
                        acc.mSubOffset[2] = acc.mParams.size();

                    /* RGBA colors */
                    else if (name == "R")
                        acc.mSubOffset[0] = acc.mParams.size();
                    else if (name == "G")
                        acc.mSubOffset[1] = acc.mParams.size();
                    else if (name == "B")
                        acc.mSubOffset[2] = acc.mParams.size();
                    else if (name == "A")
                        acc.mSubOffset[3] = acc.mParams.size();

                    /* UVWQ (STPQ) texture coordinates */
                    else if (name == "S")
                        acc.mSubOffset[0] = acc.mParams.size();
                    else if (name == "T")
                        acc.mSubOffset[1] = acc.mParams.size();
                    else if (name == "P")
                        acc.mSubOffset[2] = acc.mParams.size();
                    //  else if( name == "Q") acc.mSubOffset[3] = acc.mParams.size();
                    /* 4D uv coordinates are not supported in Assimp */

                    /* Generic extra data, interpreted as UV data, too*/
                    else if (name == "U")
                        acc.mSubOffset[0] = acc.mParams.size();
                    else if (name == "V")
                        acc.mSubOffset[1] = acc.mParams.size();
                    //else
                    //  DefaultLogger::get()->warn( format() << "Unknown accessor parameter \"" << name << "\". Ignoring data channel." );
                }

                // read data type
                int attrType = TestAttribute("type");
                if (attrType > -1) {
                    // for the moment we only distinguish between a 4x4 matrix and anything else.
                    // TODO: (thom) I don't have a spec here at work. Check if there are other multi-value types
                    // which should be tested for here.
                    std::string type = mReader->getAttributeValue(attrType);
                    if (type == "float4x4")
                        acc.mSize += 16;
                    else
                        acc.mSize += 1;
                }

                acc.mParams.push_back(name);

                // skip remaining stuff of this element, if any
                SkipElement();
            } else {
                ThrowException(format() << "Unexpected sub element <" << mReader->getNodeName() << "> in tag <accessor>");
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "accessor") != 0)
                ThrowException("Expected end of <accessor> element.");
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads input declarations of per-vertex mesh data into the given mesh
void ColladaParser::ReadVertexData(XmlNode &node, Mesh &pMesh) {
    // extract the ID of the <vertices> element. Not that we care, but to catch strange referencing schemes we should warn about
    int attrID = GetAttribute("id");
    pMesh.mVertexID = mReader->getAttributeValue(attrID);

    // a number of <input> elements
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("input")) {
                ReadInputChannel(pMesh.mPerVertexData);
            } else {
                ThrowException(format() << "Unexpected sub element <" << mReader->getNodeName() << "> in tag <vertices>");
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "vertices") != 0)
                ThrowException("Expected end of <vertices> element.");

            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads input declarations of per-index mesh data into the given mesh
void ColladaParser::ReadIndexData(XmlNode &node, Mesh &pMesh) {
    std::vector<size_t> vcount;
    std::vector<InputChannel> perIndexData;

    // read primitive count from the attribute
    int attrCount = GetAttribute("count");
    size_t numPrimitives = (size_t)mReader->getAttributeValueAsInt(attrCount);
    // some mesh types (e.g. tristrips) don't specify primitive count upfront,
    // so we need to sum up the actual number of primitives while we read the <p>-tags
    size_t actualPrimitives = 0;

    // material subgroup
    int attrMaterial = TestAttribute("material");
    SubMesh subgroup;
    if (attrMaterial > -1)
        subgroup.mMaterial = mReader->getAttributeValue(attrMaterial);

    // distinguish between polys and triangles
    std::string elementName = mReader->getNodeName();
    PrimitiveType primType = Prim_Invalid;
    if (IsElement("lines"))
        primType = Prim_Lines;
    else if (IsElement("linestrips"))
        primType = Prim_LineStrip;
    else if (IsElement("polygons"))
        primType = Prim_Polygon;
    else if (IsElement("polylist"))
        primType = Prim_Polylist;
    else if (IsElement("triangles"))
        primType = Prim_Triangles;
    else if (IsElement("trifans"))
        primType = Prim_TriFans;
    else if (IsElement("tristrips"))
        primType = Prim_TriStrips;

    ai_assert(primType != Prim_Invalid);

    // also a number of <input> elements, but in addition a <p> primitive collection and probably index counts for all primitives
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("input")) {
                ReadInputChannel(perIndexData);
            } else if (IsElement("vcount")) {
                if (!mReader->isEmptyElement()) {
                    if (numPrimitives) // It is possible to define a mesh without any primitives
                    {
                        // case <polylist> - specifies the number of indices for each polygon
                        const char *content = GetTextContent();
                        vcount.reserve(numPrimitives);
                        for (unsigned int a = 0; a < numPrimitives; a++) {
                            if (*content == 0)
                                ThrowException("Expected more values while reading <vcount> contents.");
                            // read a number
                            vcount.push_back((size_t)strtoul10(content, &content));
                            // skip whitespace after it
                            SkipSpacesAndLineEnd(&content);
                        }
                    }

                    TestClosing("vcount");
                }
            } else if (IsElement("p")) {
                if (!mReader->isEmptyElement()) {
                    // now here the actual fun starts - these are the indices to construct the mesh data from
                    actualPrimitives += ReadPrimitives(pMesh, perIndexData, numPrimitives, vcount, primType);
                }
            } else if (IsElement("extra")) {
                SkipElement("extra");
            } else if (IsElement("ph")) {
                SkipElement("ph");
            } else {
                ThrowException(format() << "Unexpected sub element <" << mReader->getNodeName() << "> in tag <" << elementName << ">");
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (mReader->getNodeName() != elementName)
                ThrowException(format() << "Expected end of <" << elementName << "> element.");

            break;
        }
    }

#ifdef ASSIMP_BUILD_DEBUG
    if (primType != Prim_TriFans && primType != Prim_TriStrips && primType != Prim_LineStrip &&
            primType != Prim_Lines) { // this is ONLY to workaround a bug in SketchUp 15.3.331 where it writes the wrong 'count' when it writes out the 'lines'.
        ai_assert(actualPrimitives == numPrimitives);
    }
#endif

    // only when we're done reading all <p> tags (and thus know the final vertex count) can we commit the submesh
    subgroup.mNumFaces = actualPrimitives;
    pMesh.mSubMeshes.push_back(subgroup);
}

// ------------------------------------------------------------------------------------------------
// Reads a single input channel element and stores it in the given array, if valid
void ColladaParser::ReadInputChannel(XmlNode &node, std::vector<InputChannel> &poChannels) {
    InputChannel channel;

    // read semantic
    int attrSemantic = GetAttribute("semantic");
    std::string semantic = mReader->getAttributeValue(attrSemantic);
    channel.mType = GetTypeForSemantic(semantic);

    // read source
    int attrSource = GetAttribute("source");
    const char *source = mReader->getAttributeValue(attrSource);
    if (source[0] != '#')
        ThrowException(format() << "Unknown reference format in url \"" << source << "\" in source attribute of <input> element.");
    channel.mAccessor = source + 1; // skipping the leading #, hopefully the remaining text is the accessor ID only

    // read index offset, if per-index <input>
    int attrOffset = TestAttribute("offset");
    if (attrOffset > -1)
        channel.mOffset = mReader->getAttributeValueAsInt(attrOffset);

    // read set if texture coordinates
    if (channel.mType == IT_Texcoord || channel.mType == IT_Color) {
        int attrSet = TestAttribute("set");
        if (attrSet > -1) {
            attrSet = mReader->getAttributeValueAsInt(attrSet);
            if (attrSet < 0)
                ThrowException(format() << "Invalid index \"" << (attrSet) << "\" in set attribute of <input> element");

            channel.mIndex = attrSet;
        }
    }

    // store, if valid type
    if (channel.mType != IT_Invalid)
        poChannels.push_back(channel);

    // skip remaining stuff of this element, if any
    SkipElement();
}

// ------------------------------------------------------------------------------------------------
// Reads a <p> primitive index list and assembles the mesh data into the given mesh
size_t ColladaParser::ReadPrimitives(XmlNode &node, Mesh &pMesh, std::vector<InputChannel> &pPerIndexChannels,
        size_t pNumPrimitives, const std::vector<size_t> &pVCount, PrimitiveType pPrimType) {
    // determine number of indices coming per vertex
    // find the offset index for all per-vertex channels
    size_t numOffsets = 1;
    size_t perVertexOffset = SIZE_MAX; // invalid value
    for (const InputChannel &channel : pPerIndexChannels) {
        numOffsets = std::max(numOffsets, channel.mOffset + 1);
        if (channel.mType == IT_Vertex)
            perVertexOffset = channel.mOffset;
    }

    // determine the expected number of indices
    size_t expectedPointCount = 0;
    switch (pPrimType) {
    case Prim_Polylist: {
        for (size_t i : pVCount)
            expectedPointCount += i;
        break;
    }
    case Prim_Lines:
        expectedPointCount = 2 * pNumPrimitives;
        break;
    case Prim_Triangles:
        expectedPointCount = 3 * pNumPrimitives;
        break;
    default:
        // other primitive types don't state the index count upfront... we need to guess
        break;
    }

    // and read all indices into a temporary array
    std::vector<size_t> indices;
    if (expectedPointCount > 0)
        indices.reserve(expectedPointCount * numOffsets);

    if (pNumPrimitives > 0) // It is possible to not contain any indices
    {
        const char *content = GetTextContent();
        while (*content != 0) {
            // read a value.
            // Hack: (thom) Some exporters put negative indices sometimes. We just try to carry on anyways.
            int value = std::max(0, strtol10(content, &content));
            indices.push_back(size_t(value));
            // skip whitespace after it
            SkipSpacesAndLineEnd(&content);
        }
    }

    // complain if the index count doesn't fit
    if (expectedPointCount > 0 && indices.size() != expectedPointCount * numOffsets) {
        if (pPrimType == Prim_Lines) {
            // HACK: We just fix this number since SketchUp 15.3.331 writes the wrong 'count' for 'lines'
            ReportWarning("Expected different index count in <p> element, %zu instead of %zu.", indices.size(), expectedPointCount * numOffsets);
            pNumPrimitives = (indices.size() / numOffsets) / 2;
        } else
            ThrowException("Expected different index count in <p> element.");

    } else if (expectedPointCount == 0 && (indices.size() % numOffsets) != 0)
        ThrowException("Expected different index count in <p> element.");

    // find the data for all sources
    for (std::vector<InputChannel>::iterator it = pMesh.mPerVertexData.begin(); it != pMesh.mPerVertexData.end(); ++it) {
        InputChannel &input = *it;
        if (input.mResolved)
            continue;

        // find accessor
        input.mResolved = &ResolveLibraryReference(mAccessorLibrary, input.mAccessor);
        // resolve accessor's data pointer as well, if necessary
        const Accessor *acc = input.mResolved;
        if (!acc->mData)
            acc->mData = &ResolveLibraryReference(mDataLibrary, acc->mSource);
    }
    // and the same for the per-index channels
    for (std::vector<InputChannel>::iterator it = pPerIndexChannels.begin(); it != pPerIndexChannels.end(); ++it) {
        InputChannel &input = *it;
        if (input.mResolved)
            continue;

        // ignore vertex pointer, it doesn't refer to an accessor
        if (input.mType == IT_Vertex) {
            // warn if the vertex channel does not refer to the <vertices> element in the same mesh
            if (input.mAccessor != pMesh.mVertexID)
                ThrowException("Unsupported vertex referencing scheme.");
            continue;
        }

        // find accessor
        input.mResolved = &ResolveLibraryReference(mAccessorLibrary, input.mAccessor);
        // resolve accessor's data pointer as well, if necessary
        const Accessor *acc = input.mResolved;
        if (!acc->mData)
            acc->mData = &ResolveLibraryReference(mDataLibrary, acc->mSource);
    }

    // For continued primitives, the given count does not come all in one <p>, but only one primitive per <p>
    size_t numPrimitives = pNumPrimitives;
    if (pPrimType == Prim_TriFans || pPrimType == Prim_Polygon)
        numPrimitives = 1;
    // For continued primitives, the given count is actually the number of <p>'s inside the parent tag
    if (pPrimType == Prim_TriStrips) {
        size_t numberOfVertices = indices.size() / numOffsets;
        numPrimitives = numberOfVertices - 2;
    }
    if (pPrimType == Prim_LineStrip) {
        size_t numberOfVertices = indices.size() / numOffsets;
        numPrimitives = numberOfVertices - 1;
    }

    pMesh.mFaceSize.reserve(numPrimitives);
    pMesh.mFacePosIndices.reserve(indices.size() / numOffsets);

    size_t polylistStartVertex = 0;
    for (size_t currentPrimitive = 0; currentPrimitive < numPrimitives; currentPrimitive++) {
        // determine number of points for this primitive
        size_t numPoints = 0;
        switch (pPrimType) {
        case Prim_Lines:
            numPoints = 2;
            for (size_t currentVertex = 0; currentVertex < numPoints; currentVertex++)
                CopyVertex(currentVertex, numOffsets, numPoints, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
            break;
        case Prim_LineStrip:
            numPoints = 2;
            for (size_t currentVertex = 0; currentVertex < numPoints; currentVertex++)
                CopyVertex(currentVertex, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
            break;
        case Prim_Triangles:
            numPoints = 3;
            for (size_t currentVertex = 0; currentVertex < numPoints; currentVertex++)
                CopyVertex(currentVertex, numOffsets, numPoints, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
            break;
        case Prim_TriStrips:
            numPoints = 3;
            ReadPrimTriStrips(numOffsets, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
            break;
        case Prim_Polylist:
            numPoints = pVCount[currentPrimitive];
            for (size_t currentVertex = 0; currentVertex < numPoints; currentVertex++)
                CopyVertex(polylistStartVertex + currentVertex, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, 0, indices);
            polylistStartVertex += numPoints;
            break;
        case Prim_TriFans:
        case Prim_Polygon:
            numPoints = indices.size() / numOffsets;
            for (size_t currentVertex = 0; currentVertex < numPoints; currentVertex++)
                CopyVertex(currentVertex, numOffsets, numPoints, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
            break;
        default:
            // LineStrip is not supported due to expected index unmangling
            ThrowException("Unsupported primitive type.");
            break;
        }

        // store the face size to later reconstruct the face from
        pMesh.mFaceSize.push_back(numPoints);
    }

    // if I ever get my hands on that guy who invented this steaming pile of indirection...
    TestClosing("p");
    return numPrimitives;
}

///@note This function won't work correctly if both PerIndex and PerVertex channels have same channels.
///For example if TEXCOORD present in both <vertices> and <polylist> tags this function will create wrong uv coordinates.
///It's not clear from COLLADA documentation is this allowed or not. For now only exporter fixed to avoid such behavior
void ColladaParser::CopyVertex(size_t currentVertex, size_t numOffsets, size_t numPoints, size_t perVertexOffset, Mesh &pMesh,
        std::vector<InputChannel> &pPerIndexChannels, size_t currentPrimitive, const std::vector<size_t> &indices) {
    // calculate the base offset of the vertex whose attributes we ant to copy
    size_t baseOffset = currentPrimitive * numOffsets * numPoints + currentVertex * numOffsets;

    // don't overrun the boundaries of the index list
    ai_assert((baseOffset + numOffsets - 1) < indices.size());

    // extract per-vertex channels using the global per-vertex offset
    for (std::vector<InputChannel>::iterator it = pMesh.mPerVertexData.begin(); it != pMesh.mPerVertexData.end(); ++it)
        ExtractDataObjectFromChannel(*it, indices[baseOffset + perVertexOffset], pMesh);
    // and extract per-index channels using there specified offset
    for (std::vector<InputChannel>::iterator it = pPerIndexChannels.begin(); it != pPerIndexChannels.end(); ++it)
        ExtractDataObjectFromChannel(*it, indices[baseOffset + it->mOffset], pMesh);

    // store the vertex-data index for later assignment of bone vertex weights
    pMesh.mFacePosIndices.push_back(indices[baseOffset + perVertexOffset]);
}

void ColladaParser::ReadPrimTriStrips(size_t numOffsets, size_t perVertexOffset, Mesh &pMesh, std::vector<InputChannel> &pPerIndexChannels,
        size_t currentPrimitive, const std::vector<size_t> &indices) {
    if (currentPrimitive % 2 != 0) {
        //odd tristrip triangles need their indices mangled, to preserve winding direction
        CopyVertex(1, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
        CopyVertex(0, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
        CopyVertex(2, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
    } else { //for non tristrips or even tristrip triangles
        CopyVertex(0, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
        CopyVertex(1, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
        CopyVertex(2, numOffsets, 1, perVertexOffset, pMesh, pPerIndexChannels, currentPrimitive, indices);
    }
}

// ------------------------------------------------------------------------------------------------
// Extracts a single object from an input channel and stores it in the appropriate mesh data array
void ColladaParser::ExtractDataObjectFromChannel(const InputChannel &pInput, size_t pLocalIndex, Mesh &pMesh) {
    // ignore vertex referrer - we handle them that separate
    if (pInput.mType == IT_Vertex)
        return;

    const Accessor &acc = *pInput.mResolved;
    if (pLocalIndex >= acc.mCount)
        ThrowException(format() << "Invalid data index (" << pLocalIndex << "/" << acc.mCount << ") in primitive specification");

    // get a pointer to the start of the data object referred to by the accessor and the local index
    const ai_real *dataObject = &(acc.mData->mValues[0]) + acc.mOffset + pLocalIndex * acc.mStride;

    // assemble according to the accessors component sub-offset list. We don't care, yet,
    // what kind of object exactly we're extracting here
    ai_real obj[4];
    for (size_t c = 0; c < 4; ++c)
        obj[c] = dataObject[acc.mSubOffset[c]];

    // now we reinterpret it according to the type we're reading here
    switch (pInput.mType) {
    case IT_Position: // ignore all position streams except 0 - there can be only one position
        if (pInput.mIndex == 0)
            pMesh.mPositions.push_back(aiVector3D(obj[0], obj[1], obj[2]));
        else
            ASSIMP_LOG_ERROR("Collada: just one vertex position stream supported");
        break;
    case IT_Normal:
        // pad to current vertex count if necessary
        if (pMesh.mNormals.size() < pMesh.mPositions.size() - 1)
            pMesh.mNormals.insert(pMesh.mNormals.end(), pMesh.mPositions.size() - pMesh.mNormals.size() - 1, aiVector3D(0, 1, 0));

        // ignore all normal streams except 0 - there can be only one normal
        if (pInput.mIndex == 0)
            pMesh.mNormals.push_back(aiVector3D(obj[0], obj[1], obj[2]));
        else
            ASSIMP_LOG_ERROR("Collada: just one vertex normal stream supported");
        break;
    case IT_Tangent:
        // pad to current vertex count if necessary
        if (pMesh.mTangents.size() < pMesh.mPositions.size() - 1)
            pMesh.mTangents.insert(pMesh.mTangents.end(), pMesh.mPositions.size() - pMesh.mTangents.size() - 1, aiVector3D(1, 0, 0));

        // ignore all tangent streams except 0 - there can be only one tangent
        if (pInput.mIndex == 0)
            pMesh.mTangents.push_back(aiVector3D(obj[0], obj[1], obj[2]));
        else
            ASSIMP_LOG_ERROR("Collada: just one vertex tangent stream supported");
        break;
    case IT_Bitangent:
        // pad to current vertex count if necessary
        if (pMesh.mBitangents.size() < pMesh.mPositions.size() - 1)
            pMesh.mBitangents.insert(pMesh.mBitangents.end(), pMesh.mPositions.size() - pMesh.mBitangents.size() - 1, aiVector3D(0, 0, 1));

        // ignore all bitangent streams except 0 - there can be only one bitangent
        if (pInput.mIndex == 0)
            pMesh.mBitangents.push_back(aiVector3D(obj[0], obj[1], obj[2]));
        else
            ASSIMP_LOG_ERROR("Collada: just one vertex bitangent stream supported");
        break;
    case IT_Texcoord:
        // up to 4 texture coord sets are fine, ignore the others
        if (pInput.mIndex < AI_MAX_NUMBER_OF_TEXTURECOORDS) {
            // pad to current vertex count if necessary
            if (pMesh.mTexCoords[pInput.mIndex].size() < pMesh.mPositions.size() - 1)
                pMesh.mTexCoords[pInput.mIndex].insert(pMesh.mTexCoords[pInput.mIndex].end(),
                        pMesh.mPositions.size() - pMesh.mTexCoords[pInput.mIndex].size() - 1, aiVector3D(0, 0, 0));

            pMesh.mTexCoords[pInput.mIndex].push_back(aiVector3D(obj[0], obj[1], obj[2]));
            if (0 != acc.mSubOffset[2] || 0 != acc.mSubOffset[3]) /* hack ... consider cleaner solution */
                pMesh.mNumUVComponents[pInput.mIndex] = 3;
        } else {
            ASSIMP_LOG_ERROR("Collada: too many texture coordinate sets. Skipping.");
        }
        break;
    case IT_Color:
        // up to 4 color sets are fine, ignore the others
        if (pInput.mIndex < AI_MAX_NUMBER_OF_COLOR_SETS) {
            // pad to current vertex count if necessary
            if (pMesh.mColors[pInput.mIndex].size() < pMesh.mPositions.size() - 1)
                pMesh.mColors[pInput.mIndex].insert(pMesh.mColors[pInput.mIndex].end(),
                        pMesh.mPositions.size() - pMesh.mColors[pInput.mIndex].size() - 1, aiColor4D(0, 0, 0, 1));

            aiColor4D result(0, 0, 0, 1);
            for (size_t i = 0; i < pInput.mResolved->mSize; ++i) {
                result[static_cast<unsigned int>(i)] = obj[pInput.mResolved->mSubOffset[i]];
            }
            pMesh.mColors[pInput.mIndex].push_back(result);
        } else {
            ASSIMP_LOG_ERROR("Collada: too many vertex color sets. Skipping.");
        }

        break;
    default:
        // IT_Invalid and IT_Vertex
        ai_assert(false && "shouldn't ever get here");
    }
}

// ------------------------------------------------------------------------------------------------
// Reads the library of node hierarchies and scene parts
void ColladaParser::ReadSceneLibrary(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            // a visual scene - generate root node under its ID and let ReadNode() do the recursive work
            if (IsElement("visual_scene")) {
                // read ID. Is optional according to the spec, but how on earth should a scene_instance refer to it then?
                int indexID = GetAttribute("id");
                const char *attrID = mReader->getAttributeValue(indexID);

                // read name if given.
                int indexName = TestAttribute("name");
                const char *attrName = "Scene";
                if (indexName > -1)
                    attrName = mReader->getAttributeValue(indexName);

                // create a node and store it in the library under its ID
                Node *node = new Node;
                node->mID = attrID;
                node->mName = attrName;
                mNodeLibrary[node->mID] = node;

                ReadSceneNode(node);
            } else {
                // ignore the rest
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "library_visual_scenes") == 0)
                //ThrowException( "Expected end of \"library_visual_scenes\" element.");

                break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a scene node's contents including children and stores it in the given node
void ColladaParser::ReadSceneNode(XmlNode &node, Node *pNode) {
    // quit immediately on <bla/> elements
    if (node.empty()) {
        return;
    }

    /*    if (mReader->isEmptyElement())
        return;*/

    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("node")) {
                Node *child = new Node;
                int attrID = TestAttribute("id");
                if (attrID > -1)
                    child->mID = mReader->getAttributeValue(attrID);
                int attrSID = TestAttribute("sid");
                if (attrSID > -1)
                    child->mSID = mReader->getAttributeValue(attrSID);

                int attrName = TestAttribute("name");
                if (attrName > -1)
                    child->mName = mReader->getAttributeValue(attrName);

                if (pNode) {
                    pNode->mChildren.push_back(child);
                    child->mParent = pNode;
                } else {
                    // no parent node given, probably called from <library_nodes> element.
                    // create new node in node library
                    mNodeLibrary[child->mID] = child;
                }

                // read on recursively from there
                ReadSceneNode(child);
                continue;
            }
            // For any further stuff we need a valid node to work on
            else if (!pNode)
                continue;

            if (IsElement("lookat"))
                ReadNodeTransformation(pNode, TF_LOOKAT);
            else if (IsElement("matrix"))
                ReadNodeTransformation(pNode, TF_MATRIX);
            else if (IsElement("rotate"))
                ReadNodeTransformation(pNode, TF_ROTATE);
            else if (IsElement("scale"))
                ReadNodeTransformation(pNode, TF_SCALE);
            else if (IsElement("skew"))
                ReadNodeTransformation(pNode, TF_SKEW);
            else if (IsElement("translate"))
                ReadNodeTransformation(pNode, TF_TRANSLATE);
            else if (IsElement("render") && pNode->mParent == nullptr && 0 == pNode->mPrimaryCamera.length()) {
                // ... scene evaluation or, in other words, postprocessing pipeline,
                // or, again in other words, a turing-complete description how to
                // render a Collada scene. The only thing that is interesting for
                // us is the primary camera.
                int attrId = TestAttribute("camera_node");
                if (-1 != attrId) {
                    const char *s = mReader->getAttributeValue(attrId);
                    if (s[0] != '#')
                        ASSIMP_LOG_ERROR("Collada: Unresolved reference format of camera");
                    else
                        pNode->mPrimaryCamera = s + 1;
                }
            } else if (IsElement("instance_node")) {
                // find the node in the library
                int attrID = TestAttribute("url");
                if (attrID != -1) {
                    const char *s = mReader->getAttributeValue(attrID);
                    if (s[0] != '#')
                        ASSIMP_LOG_ERROR("Collada: Unresolved reference format of node");
                    else {
                        pNode->mNodeInstances.push_back(NodeInstance());
                        pNode->mNodeInstances.back().mNode = s + 1;
                    }
                }
            } else if (IsElement("instance_geometry") || IsElement("instance_controller")) {
                // Reference to a mesh or controller, with possible material associations
                ReadNodeGeometry(pNode);
            } else if (IsElement("instance_light")) {
                // Reference to a light, name given in 'url' attribute
                int attrID = TestAttribute("url");
                if (-1 == attrID)
                    ASSIMP_LOG_WARN("Collada: Expected url attribute in <instance_light> element");
                else {
                    const char *url = mReader->getAttributeValue(attrID);
                    if (url[0] != '#')
                        ThrowException("Unknown reference format in <instance_light> element");

                    pNode->mLights.push_back(LightInstance());
                    pNode->mLights.back().mLight = url + 1;
                }
            } else if (IsElement("instance_camera")) {
                // Reference to a camera, name given in 'url' attribute
                int attrID = TestAttribute("url");
                if (-1 == attrID)
                    ASSIMP_LOG_WARN("Collada: Expected url attribute in <instance_camera> element");
                else {
                    const char *url = mReader->getAttributeValue(attrID);
                    if (url[0] != '#')
                        ThrowException("Unknown reference format in <instance_camera> element");

                    pNode->mCameras.push_back(CameraInstance());
                    pNode->mCameras.back().mCamera = url + 1;
                }
            } else {
                // skip everything else for the moment
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a node transformation entry of the given type and adds it to the given node's transformation list.
void ColladaParser::ReadNodeTransformation(XmlNode &node, Node *pNode, TransformType pType) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    std::string tagName = mReader->getNodeName();

    Transform tf;
    tf.mType = pType;

    // read SID
    int indexSID = TestAttribute("sid");
    if (indexSID >= 0)
        tf.mID = mReader->getAttributeValue(indexSID);

    // how many parameters to read per transformation type
    static const unsigned int sNumParameters[] = { 9, 4, 3, 3, 7, 16 };
    const char *content = GetTextContent();

    // read as many parameters and store in the transformation
    for (unsigned int a = 0; a < sNumParameters[pType]; a++) {
        // read a number
        content = fast_atoreal_move<ai_real>(content, tf.f[a]);
        // skip whitespace after it
        SkipSpacesAndLineEnd(&content);
    }

    // place the transformation at the queue of the node
    pNode->mTransforms.push_back(tf);

    // and consume the closing tag
    TestClosing(tagName.c_str());
}

// ------------------------------------------------------------------------------------------------
// Processes bind_vertex_input and bind elements
void ColladaParser::ReadMaterialVertexInputBinding(XmlNode &node, Collada::SemanticMappingTable &tbl) {
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("bind_vertex_input")) {
                Collada::InputSemanticMapEntry vn;

                // effect semantic
                int n = GetAttribute("semantic");
                std::string s = mReader->getAttributeValue(n);

                // input semantic
                n = GetAttribute("input_semantic");
                vn.mType = GetTypeForSemantic(mReader->getAttributeValue(n));

                // index of input set
                n = TestAttribute("input_set");
                if (-1 != n)
                    vn.mSet = mReader->getAttributeValueAsInt(n);

                tbl.mMap[s] = vn;
            } else if (IsElement("bind")) {
                ASSIMP_LOG_WARN("Collada: Found unsupported <bind> element");
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (strcmp(mReader->getNodeName(), "instance_material") == 0)
                break;
        }
    }
}

void ColladaParser::ReadEmbeddedTextures(ZipArchiveIOSystem &zip_archive) {
    // Attempt to load any undefined Collada::Image in ImageLibrary
    for (ImageLibrary::iterator it = mImageLibrary.begin(); it != mImageLibrary.end(); ++it) {
        Collada::Image &image = (*it).second;

        if (image.mImageData.empty()) {
            std::unique_ptr<IOStream> image_file(zip_archive.Open(image.mFileName.c_str()));
            if (image_file) {
                image.mImageData.resize(image_file->FileSize());
                image_file->Read(image.mImageData.data(), image_file->FileSize(), 1);
                image.mEmbeddedFormat = BaseImporter::GetExtension(image.mFileName);
                if (image.mEmbeddedFormat == "jpeg") {
                    image.mEmbeddedFormat = "jpg";
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Reads a mesh reference in a node and adds it to the node's mesh list
void ColladaParser::ReadNodeGeometry(XmlNode &node, Node *pNode) {
    // referred mesh is given as an attribute of the <instance_geometry> element
    int attrUrl = GetAttribute("url");
    const char *url = mReader->getAttributeValue(attrUrl);
    if (url[0] != '#')
        ThrowException("Unknown reference format");

    Collada::MeshInstance instance;
    instance.mMeshOrController = url + 1; // skipping the leading #

    if (!mReader->isEmptyElement()) {
        // read material associations. Ignore additional elements in between
        while (mReader->read()) {
            if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
                if (IsElement("instance_material")) {
                    // read ID of the geometry subgroup and the target material
                    int attrGroup = GetAttribute("symbol");
                    std::string group = mReader->getAttributeValue(attrGroup);
                    int attrMaterial = GetAttribute("target");
                    const char *urlMat = mReader->getAttributeValue(attrMaterial);
                    Collada::SemanticMappingTable s;
                    if (urlMat[0] == '#')
                        urlMat++;

                    s.mMatName = urlMat;

                    // resolve further material details + THIS UGLY AND NASTY semantic mapping stuff
                    if (!mReader->isEmptyElement())
                        ReadMaterialVertexInputBinding(s);

                    // store the association
                    instance.mMaterials[group] = s;
                }
            } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
                if (strcmp(mReader->getNodeName(), "instance_geometry") == 0 || strcmp(mReader->getNodeName(), "instance_controller") == 0)
                    break;
            }
        }
    }

    // store it
    pNode->mMeshes.push_back(instance);
}

// ------------------------------------------------------------------------------------------------
// Reads the collada scene
void ColladaParser::ReadScene(XmlNode &node) {
    if (node.empty()) {
        return;
    }
    /*if (mReader->isEmptyElement())
        return;*/

    for (XmlNode currentNode : node.children()) {
        const std::string currentName = currentNode.name();
        if (currentName == "instance_visual_scene") {
            // should be the first and only occurrence
            if (mRootNode)
                ThrowException("Invalid scene containing multiple root nodes in <instance_visual_scene> element");

            // read the url of the scene to instance. Should be of format "#some_name"
            int urlIndex = currentNode.attribute("url").as_int();
            const char *url = currentNode.attributes.begin() + urlIndex;
            if (url[0] != '#') {
                ThrowException("Unknown reference format in <instance_visual_scene> element");
            }

            // find the referred scene, skip the leading #
            NodeLibrary::const_iterator sit = mNodeLibrary.find(url + 1);
            if (sit == mNodeLibrary.end()) {
                ThrowException("Unable to resolve visual_scene reference \"" + std::string(url) + "\" in <instance_visual_scene> element.");
            }
            mRootNode = sit->second;
        }
    }

    /*while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT) {
            if (IsElement("instance_visual_scene")) {
                // should be the first and only occurrence
                if (mRootNode)
                    ThrowException("Invalid scene containing multiple root nodes in <instance_visual_scene> element");

                // read the url of the scene to instance. Should be of format "#some_name"
                int urlIndex = GetAttribute("url");
                const char *url = mReader->getAttributeValue(urlIndex);
                if (url[0] != '#')
                    ThrowException("Unknown reference format in <instance_visual_scene> element");

                // find the referred scene, skip the leading #
                NodeLibrary::const_iterator sit = mNodeLibrary.find(url + 1);
                if (sit == mNodeLibrary.end())
                    ThrowException("Unable to resolve visual_scene reference \"" + std::string(url) + "\" in <instance_visual_scene> element.");
                mRootNode = sit->second;
            } else {
                SkipElement();
            }
        } else if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            break;
        }
    }*/
}

// ------------------------------------------------------------------------------------------------
// Aborts the file reading with an exception
AI_WONT_RETURN void ColladaParser::ThrowException(const std::string &pError) const {
    throw DeadlyImportError(format() << "Collada: " << mFileName << " - " << pError);
}

void ColladaParser::ReportWarning(const char *msg, ...) {
    ai_assert(nullptr != msg);

    va_list args;
    va_start(args, msg);

    char szBuffer[3000];
    const int iLen = vsprintf(szBuffer, msg, args);
    ai_assert(iLen > 0);

    va_end(args);
    ASSIMP_LOG_WARN_F("Validation warning: ", std::string(szBuffer, iLen));
}

// ------------------------------------------------------------------------------------------------
// Skips all data until the end node of the current element
/*void ColladaParser::SkipElement() {
    // nothing to skip if it's an <element />
    if (mReader->isEmptyElement()) {
        return;
    }

    // reroute
    SkipElement(mReader->getNodeName());
}*/

// ------------------------------------------------------------------------------------------------
// Skips all data until the end node of the given element
/*void ColladaParser::SkipElement(const char *pElement) {
    // copy the current node's name because it'a pointer to the reader's internal buffer,
    // which is going to change with the upcoming parsing
    std::string element = pElement;
    while (mReader->read()) {
        if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END) {
            if (mReader->getNodeName() == element) {
                break;
            }
        }
    }
}*/

// ------------------------------------------------------------------------------------------------
// Tests for an opening element of the given name, throws an exception if not found
/*void ColladaParser::TestOpening(const char *pName) {
    // read element start
    if (!mReader->read()) {
        ThrowException(format() << "Unexpected end of file while beginning of <" << pName << "> element.");
    }
    // whitespace in front is ok, just read again if found
    if (mReader->getNodeType() == irr::io::EXN_TEXT) {
        if (!mReader->read()) {
            ThrowException(format() << "Unexpected end of file while reading beginning of <" << pName << "> element.");
        }
    }

    if (mReader->getNodeType() != irr::io::EXN_ELEMENT || strcmp(mReader->getNodeName(), pName) != 0) {
        ThrowException(format() << "Expected start of <" << pName << "> element.");
    }
}*/

// ------------------------------------------------------------------------------------------------
// Tests for the closing tag of the given element, throws an exception if not found
/*void ColladaParser::TestClosing(const char *pName) {
    // check if we have an empty (self-closing) element
    if (mReader->isEmptyElement()) {
        return;
    }

    // check if we're already on the closing tag and return right away
    if (mReader->getNodeType() == irr::io::EXN_ELEMENT_END && strcmp(mReader->getNodeName(), pName) == 0) {
        return;
    }

    // if not, read some more
    if (!mReader->read()) {
        ThrowException(format() << "Unexpected end of file while reading end of <" << pName << "> element.");
    }
    // whitespace in front is ok, just read again if found
    if (mReader->getNodeType() == irr::io::EXN_TEXT) {
        if (!mReader->read()) {
            ThrowException(format() << "Unexpected end of file while reading end of <" << pName << "> element.");
        }
    }

    // but this has the be the closing tag, or we're lost
    if (mReader->getNodeType() != irr::io::EXN_ELEMENT_END || strcmp(mReader->getNodeName(), pName) != 0) {
        ThrowException(format() << "Expected end of <" << pName << "> element.");
    }
}*/

// ------------------------------------------------------------------------------------------------
// Returns the index of the named attribute or -1 if not found. Does not throw, therefore useful for optional attributes
/*int ColladaParser::GetAttribute(const char *pAttr) const {
    int index = TestAttribute(pAttr);
    if (index == -1) {
        ThrowException(format() << "Expected attribute \"" << pAttr << "\" for element <" << mReader->getNodeName() << ">.");
    }

    // attribute not found -> throw an exception
    return index;
}*/

// ------------------------------------------------------------------------------------------------
// Tests the present element for the presence of one attribute, returns its index or throws an exception if not found
/*int ColladaParser::TestAttribute(const char *pAttr) const {
    for (int a = 0; a < mReader->getAttributeCount(); a++)
        if (strcmp(mReader->getAttributeName(a), pAttr) == 0)
            return a;

    return -1;
}*/

// ------------------------------------------------------------------------------------------------
// Reads the text contents of an element, throws an exception if not given. Skips leading whitespace.
/*const char *ColladaParser::GetTextContent() {
    const char *sz = TestTextContent();
    if (!sz) {
        ThrowException("Invalid contents in element \"n\".");
    }
    return sz;
}*/

// ------------------------------------------------------------------------------------------------
// Reads the text contents of an element, returns nullptr if not given. Skips leading whitespace.
/*const char *ColladaParser::TestTextContent() {
    // present node should be the beginning of an element
    if (mReader->getNodeType() != irr::io::EXN_ELEMENT || mReader->isEmptyElement())
        return nullptr;

    // read contents of the element
    if (!mReader->read()) {
        return nullptr;
    }
    if (mReader->getNodeType() != irr::io::EXN_TEXT && mReader->getNodeType() != irr::io::EXN_CDATA) {
        return nullptr;
    }

    // skip leading whitespace
    const char *text = mReader->getNodeData();
    SkipSpacesAndLineEnd(&text);

    return text;
}*/

// ------------------------------------------------------------------------------------------------
// Calculates the resulting transformation from all the given transform steps
aiMatrix4x4 ColladaParser::CalculateResultTransform(const std::vector<Transform> &pTransforms) const {
    aiMatrix4x4 res;

    for (std::vector<Transform>::const_iterator it = pTransforms.begin(); it != pTransforms.end(); ++it) {
        const Transform &tf = *it;
        switch (tf.mType) {
        case TF_LOOKAT: {
            aiVector3D pos(tf.f[0], tf.f[1], tf.f[2]);
            aiVector3D dstPos(tf.f[3], tf.f[4], tf.f[5]);
            aiVector3D up = aiVector3D(tf.f[6], tf.f[7], tf.f[8]).Normalize();
            aiVector3D dir = aiVector3D(dstPos - pos).Normalize();
            aiVector3D right = (dir ^ up).Normalize();

            res *= aiMatrix4x4(
                    right.x, up.x, -dir.x, pos.x,
                    right.y, up.y, -dir.y, pos.y,
                    right.z, up.z, -dir.z, pos.z,
                    0, 0, 0, 1);
            break;
        }
        case TF_ROTATE: {
            aiMatrix4x4 rot;
            ai_real angle = tf.f[3] * ai_real(AI_MATH_PI) / ai_real(180.0);
            aiVector3D axis(tf.f[0], tf.f[1], tf.f[2]);
            aiMatrix4x4::Rotation(angle, axis, rot);
            res *= rot;
            break;
        }
        case TF_TRANSLATE: {
            aiMatrix4x4 trans;
            aiMatrix4x4::Translation(aiVector3D(tf.f[0], tf.f[1], tf.f[2]), trans);
            res *= trans;
            break;
        }
        case TF_SCALE: {
            aiMatrix4x4 scale(tf.f[0], 0.0f, 0.0f, 0.0f, 0.0f, tf.f[1], 0.0f, 0.0f, 0.0f, 0.0f, tf.f[2], 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);
            res *= scale;
            break;
        }
        case TF_SKEW:
            // TODO: (thom)
            ai_assert(false);
            break;
        case TF_MATRIX: {
            aiMatrix4x4 mat(tf.f[0], tf.f[1], tf.f[2], tf.f[3], tf.f[4], tf.f[5], tf.f[6], tf.f[7],
                    tf.f[8], tf.f[9], tf.f[10], tf.f[11], tf.f[12], tf.f[13], tf.f[14], tf.f[15]);
            res *= mat;
            break;
        }
        default:
            ai_assert(false);
            break;
        }
    }

    return res;
}

// ------------------------------------------------------------------------------------------------
// Determines the input data type for the given semantic string
Collada::InputType ColladaParser::GetTypeForSemantic(const std::string &semantic) {
    if (semantic.empty()) {
        ASSIMP_LOG_WARN("Vertex input type is empty.");
        return IT_Invalid;
    }

    if (semantic == "POSITION")
        return IT_Position;
    else if (semantic == "TEXCOORD")
        return IT_Texcoord;
    else if (semantic == "NORMAL")
        return IT_Normal;
    else if (semantic == "COLOR")
        return IT_Color;
    else if (semantic == "VERTEX")
        return IT_Vertex;
    else if (semantic == "BINORMAL" || semantic == "TEXBINORMAL")
        return IT_Bitangent;
    else if (semantic == "TANGENT" || semantic == "TEXTANGENT")
        return IT_Tangent;

    ASSIMP_LOG_WARN_F("Unknown vertex input type \"", semantic, "\". Ignoring.");
    return IT_Invalid;
}

#endif // !! ASSIMP_BUILD_NO_DAE_IMPORTER
