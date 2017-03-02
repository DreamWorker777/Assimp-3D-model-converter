/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2016, assimp team

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

#ifndef ASSIMP_BUILD_NO_MMD_IMPORTER

#include "DefaultIOSystem.h"
#include "MMDImporter.h"
#include "MMDPmxParser.h"
#include "MMDPmdParser.h"
#include "MMDVmdParser.h"
//#include "IOStreamBuffer.h"
#include "ConvertToLHProcess.h"
#include <memory>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/ai_assert.h>
#include <assimp/DefaultLogger.hpp>
#include <fstream>
#include <iomanip>

static const aiImporterDesc desc = {
    "MMD Importer",
    "",
    "",
    "surfaces supported?",
    aiImporterFlags_SupportTextFlavour,
    0,
    0,
    0,
    0,
    "pmx"
};

namespace Assimp {

using namespace std;

// ------------------------------------------------------------------------------------------------
//  Default constructor
MMDImporter::MMDImporter() :
    m_Buffer(),
    //m_pRootObject( NULL ),
    m_strAbsPath( "" )
{
    DefaultIOSystem io;
    m_strAbsPath = io.getOsSeparator();
}

// ------------------------------------------------------------------------------------------------
//  Destructor.
MMDImporter::~MMDImporter()
{
    //delete m_pRootObject;
    //m_pRootObject = NULL;
}

// ------------------------------------------------------------------------------------------------
//  Returns true, if file is an pmx file.
bool MMDImporter::CanRead( const std::string& pFile, IOSystem*  pIOHandler , bool checkSig ) const
{
    if(!checkSig) //Check File Extension
    {
        return SimpleExtensionCheck(pFile,"pmx");
    }
    else //Check file Header
    {
        static const char *pTokens[] = { "PMX " };
        return BaseImporter::SearchFileHeaderForToken(pIOHandler, pFile, pTokens, 1 );
    }
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc* MMDImporter::GetInfo () const
{
    return &desc;
}

// ------------------------------------------------------------------------------------------------
//  MMD import implementation
void MMDImporter::InternReadFile( const std::string &file, aiScene* pScene, IOSystem* pIOHandler)
{
    // Read file by istream
    std::filebuf fb;
    if( !fb.open(file, std::ios::in | std::ios::binary ) ) {
        throw DeadlyImportError( "Failed to open file " + file + "." );
    }

    std::istream fileStream( &fb );

    // Get the file-size and validate it, throwing an exception when fails
    fileStream.seekg(0, fileStream.end);
    size_t fileSize = fileStream.tellg();
    fileStream.seekg(0, fileStream.beg);

    if( fileSize < sizeof(pmx::PmxModel) ) {
        throw DeadlyImportError( file + " is too small." );
    }

    pmx::PmxModel model;
    model.Read(&fileStream);

    CreateDataFromImport(&model, pScene);
}

// ------------------------------------------------------------------------------------------------
void MMDImporter::CreateDataFromImport(const pmx::PmxModel* pModel, aiScene* pScene)
{
    if( pModel == NULL ) {
        return;
    }

    aiNode *pNode = new aiNode;
    if ( !pModel->model_name.empty() ) {
        pNode->mName.Set(pModel->model_name);
    }
    else {
        ai_assert(false);
    }
    
    pScene->mRootNode = pNode;
    std::cout << pScene->mRootNode->mName.C_Str() << std::endl;
    std::cout << pModel->index_count << std::endl;

    pNode = new aiNode;
    pScene->mRootNode->addChildren(1, &pNode);
    pScene->mRootNode->mNumChildren = 1;
    pNode->mParent = pScene->mRootNode;
    pNode->mName.Set(string(pModel->model_name) + string("_mesh"));

    // split mesh by materials
    pNode->mNumMeshes = pModel->material_count;
    pNode->mMeshes = new unsigned int[pNode->mNumMeshes];
    for( unsigned int index = 0; index < pNode->mNumMeshes; index++ ) {
        pNode->mMeshes[index] = index;
    }

    pScene->mNumMeshes = pNode->mNumMeshes;
    pScene->mMeshes = new aiMesh*[pScene->mNumMeshes];
    for( unsigned int i = 0, indexStart = 0; i < pScene->mNumMeshes; i++ ) {
        const int indexCount = pModel->materials[i].index_count;

        std::cout << pModel->materials[i].material_name << std::endl;
        std::cout << indexStart << " " << indexCount << std::endl;

        pScene->mMeshes[i] = CreateMesh(pModel,  indexStart, indexCount);
        pScene->mMeshes[i]->mName = pModel->materials[i].material_name;
        pScene->mMeshes[i]->mMaterialIndex = i;
        indexStart += indexCount;
    }

    // create textures, may be dummy?
    /*
    pScene->mNumTextures = pModel->texture_count;
    pScene->mTextures = new aiTexture*[pScene->mNumTextures];
    for( unsigned int i = 0; i < pScene->mNumTextures; ++i) {
        aiTexture *tex = new aiTexture;        
        pScene->mTextures[i] = tex;
        strcpy(tex->achFormatHint, "png");
        tex->mHeight = 0;
        ifstream file(pModel->textures[i], ios::binary | ios::ate);
        streamsize size = file.tellg();
        file.seekg(0, ios::beg);
        char *buffer = new char[size];
        file.read(buffer, size);
        if(file.bad()) {
            string err("PMX: Can't open texture file");
            err.append(pModel->textures[i]);
            throw DeadlyExportError(err);
        }
        tex->pcData = (aiTexel*)buffer; 
    }
    */

    // create materials
    pScene->mNumMaterials = pModel->material_count;
    pScene->mMaterials = new aiMaterial*[pScene->mNumMaterials];
    for( unsigned int i = 0; i < pScene->mNumMaterials; i++ ) {
        pScene->mMaterials[i] = CreateMaterial(&pModel->materials[i], pModel);
    }

    // Convert everything to OpenGL space
    MakeLeftHandedProcess convertProcess;
    convertProcess.Execute( pScene);

    FlipWindingOrderProcess flipper;
    flipper.Execute(pScene);

}

// ------------------------------------------------------------------------------------------------
aiMesh* MMDImporter::CreateMesh(const pmx::PmxModel* pModel, const int indexStart, const int indexCount)
{
    aiMesh *pMesh = new aiMesh;

    pMesh->mNumVertices = indexCount;

    pMesh->mNumFaces = indexCount / 3;
    pMesh->mFaces = new aiFace[ pMesh->mNumFaces ];

    for( unsigned int index = 0; index < pMesh->mNumFaces; index++ ) {
        const int numIndices = 3; // trianglular face
        pMesh->mFaces[index].mNumIndices = numIndices;
        unsigned int *indices = new unsigned int[numIndices];
        indices[0] = numIndices * index;
        indices[1] = numIndices * index + 1;
        indices[2] = numIndices * index + 2;
        pMesh->mFaces[index].mIndices = indices;
    }

    pMesh->mVertices = new aiVector3D[ pMesh->mNumVertices ];
    pMesh->mNormals = new aiVector3D[ pMesh->mNumVertices ];
    pMesh->mTextureCoords[0] = new aiVector3D[ pMesh->mNumVertices ];
    pMesh->mNumUVComponents[0] = 2;

    // additional UVs
    for( int i = 1; i <= pModel->setting.uv; i++ ) {
        pMesh->mTextureCoords[i] = new aiVector3D[ pMesh->mNumVertices ];
        pMesh->mNumUVComponents[i] = 4;
    }

    for( int index = 0; index < indexCount; index++ ) {
        const pmx::PmxVertex *v = &pModel->vertices[pModel->indices[indexStart + index]];
        const float* position = v->position;
        pMesh->mVertices[index].Set(position[0], position[1], position[2]);
        const float* normal = v->normal;
        pMesh->mNormals[index].Set(normal[0], normal[1], normal[2]);
        pMesh->mTextureCoords[0][index].x = v->uv[0];
        pMesh->mTextureCoords[0][index].y = -v->uv[1];
        for( int i = 1; i <= pModel->setting.uv; i++ ) {
            // TODO: wrong here? use quaternion transform?
            pMesh->mTextureCoords[i][index].x = v->uva[i][0];
            pMesh->mTextureCoords[i][index].y = v->uva[i][1];
        }
    }

    return pMesh;
}

// ------------------------------------------------------------------------------------------------
aiMaterial* MMDImporter::CreateMaterial(const pmx::PmxMaterial* pMat, const pmx::PmxModel* pModel)
{
    aiMaterial *mat = new aiMaterial();
    aiString name(pMat->material_english_name);
    mat->AddProperty(&name, AI_MATKEY_NAME);

    aiColor3D diffuse(pMat->diffuse[0], pMat->diffuse[1], pMat->diffuse[2]);
    mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
    aiColor3D specular(pMat->specular[0], pMat->specular[1], pMat->specular[2]);
    mat->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
    aiColor3D ambient(pMat->ambient[0], pMat->ambient[1], pMat->ambient[2]);
    mat->AddProperty(&ambient, 1, AI_MATKEY_COLOR_AMBIENT);

    float opacity = pMat->diffuse[3];
    mat->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
    float shininess = pMat->specularlity;
    mat->AddProperty(&shininess, 1, AI_MATKEY_SHININESS_STRENGTH);
    
    aiString texture_path(pModel->textures[pMat->diffuse_texture_index]);
    mat->AddProperty(&texture_path, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, 0));
    int mapping_uvwsrc = 0;
    mat->AddProperty(&mapping_uvwsrc, 1, AI_MATKEY_UVWSRC(aiTextureType_DIFFUSE, 0));
    int mapping_mode = aiTextureMapMode_Mirror;
    mat->AddProperty(&mapping_mode, 1, AI_MATKEY_MAPPINGMODE_U(aiTextureType_DIFFUSE, 0));
    mat->AddProperty(&mapping_mode, 1, AI_MATKEY_MAPPINGMODE_V(aiTextureType_DIFFUSE, 0));

    return mat;
}

// ------------------------------------------------------------------------------------------------

}   // Namespace Assimp

#endif // !! ASSIMP_BUILD_NO_MMD_IMPORTER
