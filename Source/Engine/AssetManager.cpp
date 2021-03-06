#include "AssetManager.h"

#include <filesystem>

#include <3rdParty/LodePNG/lodepng.h>
#include <3rdParty/tinyobjloader/tiny_obj_loader.h>
#pragma warning(disable:4244) // warning C4244: 'initializing': conversion from 'double' to 'float', possible loss of data
#include <3rdParty/OBJ_Loader/OBJ_Loader.h>
#pragma warning(default:4244)

#include <Driver/ITexture.h>
#include <Driver/IBuffer.h>
#include <Util/ThreadPool.h>
#include <Renderer/WorldRenderer.h>

#include "Material.h"
#include "MeshRenderer.h"
#include "VertexData.h"

static constexpr bool ASYNC_LOADING_ENABLED = true;

AssetManager::AssetManager()
{
	initInis();
	initShaders();
	initDefaultAssets();
}

AssetManager::~AssetManager()
{
	unloadCurrentScene();

	defaultMeshVb.reset();
	defaultMeshIb.reset();

	for (ITexture* t : engineTextures)
		delete t;
	engineTextures.clear();

	standardInputLayout.close();
	for (ResId& id : standardShaders)
	{
		drv->destroyResource(id);
		id = BAD_RESID;
	}
}

ITexture* AssetManager::loadTextureFromPng(const std::string& path, bool srgb, LoadExecutionMode lem)
{
	ITexture* texture = drv->createTextureStub();

	auto load = [path, srgb, texture]
	{
		PLOG_INFO << "Loading texture from file: " << path;

		std::vector<unsigned char> pngData;
		unsigned int width, height;
		unsigned int error = lodepng::decode(pngData, width, height, path);
		if (error != 0)
		{
			PLOG_ERROR << "Error loading texture." << std::endl
				<< "\tFile: " << path << std::endl
				<< "\tError: " << lodepng_error_text(error);
			return;
		}

		TextureDesc tDesc(path, width, height, srgb ? TexFmt::R8G8B8A8_UNORM_SRGB : TexFmt::R8G8B8A8_UNORM, 0);
		tDesc.bindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
		tDesc.miscFlags = RESOURCE_MISC_GENERATE_MIPS;
		texture->recreate(tDesc);
		texture->updateData(0, nullptr, (void*)pngData.data());
		texture->generateMips();
	};

	if (lem == LoadExecutionMode::ASYNC && ASYNC_LOADING_ENABLED)
		tp->enqueue(load);
	else
		load();

	return texture;
}

bool AssetManager::loadTexturesToStandardMaterial(const MaterialTexturePaths& paths, Material* material, bool flip_normal_green, LoadExecutionMode lem)
{
	ITexture* baseTexture = drv->createTextureStub();
	ITexture* normalRoughMetalTexture = drv->createTextureStub();
	material->setTexture(ShaderStage::PS, 0, baseTexture, MaterialTexture::Purpose::COLOR);
	material->setTexture(ShaderStage::PS, 1, normalRoughMetalTexture, MaterialTexture::Purpose::NORMAL);
	material->setKeyword("ALPHA_TEST_ON", !paths.opacity.empty());

	auto load = [paths, material, flip_normal_green, baseTexture, normalRoughMetalTexture]
	{
		auto loadTex = [&](const std::string& path, std::vector<unsigned char>& data, unsigned int& width, unsigned int& height)
		{
			PLOG_DEBUG << "Loading texture from file: " << path;
			unsigned int error = lodepng::decode(data, width, height, path);
			if (error != 0)
			{
				PLOG_ERROR << "Error loading texture." << std::endl
					<< "\tFile: " << path << std::endl
					<< "\tError: " << lodepng_error_text(error);
				return false;
			}
			return true;
		};

		static constexpr int NUM_CHANNELS = 4;

		bool hasAlbedo = !paths.albedo.empty();
		bool hasOpacity = !paths.opacity.empty();
		bool hasSeparateOpacity = hasOpacity && paths.albedo != paths.opacity;

		if (hasAlbedo || hasOpacity)
		{
			unsigned int albedoOpacityWidth = 0, albedoOpacityHeight = 0;

			std::vector<unsigned char> albedoData;
			unsigned int albedoWidth = 0, albedoHeight = 0;
			if (hasAlbedo)
			{
				if (!loadTex(paths.albedo, albedoData, albedoWidth, albedoHeight))
					return;
				albedoOpacityWidth = albedoWidth;
				albedoOpacityHeight = albedoHeight;
			}

			std::vector<unsigned char> opacityData;
			unsigned int opacityWidth = 0, opacityHeight = 0;
			if (hasSeparateOpacity)
			{
				if (!loadTex(paths.opacity, opacityData, opacityWidth, opacityHeight))
					return;
				if (hasAlbedo && (opacityWidth != albedoOpacityWidth || opacityHeight != albedoOpacityHeight))
				{
					PLOG_ERROR << "Mismatching dimensions of albedo and opacity texture." << std::endl
						<< "\tAlbedo: " << paths.albedo << " (" << albedoOpacityWidth << "x" << albedoOpacityHeight << ")" << std::endl
						<< "\tOpacity: " << paths.opacity << " (" << opacityWidth << "x" << opacityHeight << ")";
					return;
				}
				albedoOpacityWidth = opacityWidth;
				albedoOpacityHeight = opacityHeight;
			}

			const int numTexels = albedoOpacityWidth * albedoOpacityHeight;
			const int numBytes = numTexels * NUM_CHANNELS;

			if (!hasAlbedo)
				albedoData.assign(numBytes, 127u);
			if (!hasOpacity)
				opacityData.assign(numBytes, 255u);

			std::vector<unsigned char> albedoOpacityData;
			albedoOpacityData.resize(numBytes);
			for (int i = 0; i < numTexels; i++)
			{
				int r = i * NUM_CHANNELS + 0;
				int g = i * NUM_CHANNELS + 1;
				int b = i * NUM_CHANNELS + 2;
				int a = i * NUM_CHANNELS + 3;
				albedoOpacityData[r] = albedoData[r];
				albedoOpacityData[g] = albedoData[g];
				albedoOpacityData[b] = albedoData[b];
				albedoOpacityData[a] = hasSeparateOpacity ? opacityData[r] : albedoData[a];
			}

			TextureDesc baseTexDesc(material->name + "_base", albedoWidth, albedoHeight, TexFmt::R8G8B8A8_UNORM_SRGB, 0);
			baseTexDesc.bindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
			baseTexDesc.miscFlags = RESOURCE_MISC_GENERATE_MIPS;
			baseTexture->recreate(baseTexDesc);
			baseTexture->updateData(0, nullptr, (void*)albedoOpacityData.data());
			baseTexture->generateMips();
		}

		bool hasNormal = !paths.normal.empty();
		bool hasRoughness = !paths.roughness.empty();
		bool hasMetalness = !paths.metalness.empty();

		if (!hasNormal && !hasRoughness && !hasMetalness)
			return;

		unsigned int normalRoughMetalWidth = 0, normalRoughMetalHeight = 0;

		std::vector<unsigned char> normalData;
		unsigned int normalWidth = 0, normalHeight = 0;
		if (hasNormal)
		{
			if (!loadTex(paths.normal, normalData, normalWidth, normalHeight))
				return;
			normalRoughMetalWidth = normalWidth;
			normalRoughMetalHeight = normalHeight;
		}

		std::vector<unsigned char> roughnessData;
		unsigned int roughnessWidth = 0, roughnessHeight = 0;
		if (hasRoughness)
		{
			if (!loadTex(paths.roughness, roughnessData, roughnessWidth, roughnessHeight))
				return;
			if (hasNormal && (roughnessWidth != normalRoughMetalWidth || roughnessHeight != normalRoughMetalHeight))
			{
				PLOG_ERROR << "Mismatching dimensions of normal and roughness texture." << std::endl
					<< "\tNormal: " << paths.normal << " (" << normalRoughMetalWidth << "x" << normalRoughMetalHeight << ")" << std::endl
					<< "\tRougness: " << paths.roughness << " (" << roughnessWidth << "x" << roughnessHeight << ")";
				return;
			}
			normalRoughMetalWidth = roughnessWidth;
			normalRoughMetalHeight = roughnessHeight;
		}

		std::vector<unsigned char> metalnessData;
		unsigned int metalnessWidth = 0, metalnessHeight = 0;
		if (hasMetalness)
		{
			if (!loadTex(paths.metalness, metalnessData, metalnessWidth, metalnessHeight))
				return;
			if ((hasNormal || hasRoughness) && (metalnessWidth != normalRoughMetalWidth || metalnessHeight != normalRoughMetalHeight))
			{
				PLOG_ERROR << "Mismatching dimensions of metalness and normal or roughness texture." << std::endl
					<< "\tNormal or roughness: " << paths.normal << " (" << normalRoughMetalWidth << "x" << normalRoughMetalHeight << ")" << std::endl
					<< "\tMetalness: " << paths.metalness << " (" << metalnessWidth << "x" << metalnessHeight << ")";
				return;
			}
			normalRoughMetalWidth = metalnessWidth;
			normalRoughMetalHeight = metalnessHeight;
		}

		assert(!hasNormal || !hasRoughness || normalData.size() == roughnessData.size());
		assert(!hasNormal || !hasMetalness || normalData.size() == metalnessData.size());
		assert(!hasRoughness || !hasMetalness || roughnessData.size() == metalnessData.size());

		const int numTexels = normalRoughMetalWidth * normalRoughMetalHeight;
		const int numBytes = numTexels * NUM_CHANNELS;

		if (!hasNormal)
			normalData.assign(numBytes, 127u);
		if (!hasRoughness)
			roughnessData.assign(numBytes, 127u);
		if (!hasMetalness)
			metalnessData.assign(numBytes, 0);

		std::vector<unsigned char> normalRoughMetalData;
		normalRoughMetalData.resize(numBytes);
		for (int i = 0; i < numTexels; i++)
		{
			int r = i * NUM_CHANNELS + 0;
			int g = i * NUM_CHANNELS + 1;
			int b = i * NUM_CHANNELS + 2;
			int a = i * NUM_CHANNELS + 3;
			normalRoughMetalData[r] = normalData[r];
			normalRoughMetalData[g] = flip_normal_green ? 255u - normalData[g] : normalData[g];
			normalRoughMetalData[b] = metalnessData[r];
			normalRoughMetalData[a] = roughnessData[r];
		}

		TextureDesc normalRoughMetalTexDesc(material->name + "_normRoughMetal", normalWidth, normalHeight, TexFmt::R8G8B8A8_UNORM, 0);
		normalRoughMetalTexDesc.bindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
		normalRoughMetalTexDesc.miscFlags = RESOURCE_MISC_GENERATE_MIPS;
		normalRoughMetalTexture->recreate(normalRoughMetalTexDesc);
		normalRoughMetalTexture->updateData(0, nullptr, (void*)normalRoughMetalData.data());
		normalRoughMetalTexture->generateMips();
	};

	if (lem == LoadExecutionMode::ASYNC && ASYNC_LOADING_ENABLED)
		tp->enqueue(load);
	else
		load();

	return true;
}

bool AssetManager::loadMesh(const std::string& name, MeshData& mesh_data)
{
	if (!modelsIni.has(name))
	{
		PLOG_ERROR << "No model found with name: " << name;
		return false;
	}

	std::string path = modelsIni[name]["path"];
	std::string dir = std::filesystem::path(path).parent_path().u8string();
	float importScale = modelsIni[name]["scale"].length() > 0 ? std::stof(modelsIni[name]["scale"]) : 1.0f;
	bool flipUvX = modelsIni[name]["flipUvX"] == "yes";
	bool flipUvY = modelsIni[name]["flipUvY"] == "yes";

	PLOG_INFO << "Loading mesh '" << name << "' from file: " << path.c_str();

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> mtls;

	std::string warn;
	std::string err;

	auto startLoadTime = std::chrono::high_resolution_clock::now();
	bool success = tinyobj::LoadObj(&attrib, &shapes, &mtls, &warn, &err, path.c_str(), dir.c_str());
	auto finishLoadTime = std::chrono::high_resolution_clock::now();
	if (!warn.empty())
		PLOG_WARNING << warn;
	if (!err.empty())
		PLOG_ERROR << err;
	if (!success)
		return false;
	PLOG_INFO << "Loading mesh '" << name << "' successful. It took " << (finishLoadTime - startLoadTime).count() / 1e9 << " seconds. Processing...";

	// Loop over materials
	std::vector<Material*> materials;
	for (size_t m = 0; m < mtls.size(); m++)
	{
		const tinyobj::material_t& mtl = mtls[m];

		Material* material = new Material(name + "__" + mtl.name, standardShaders);

		MaterialTexturePaths paths;
		if (!mtl.diffuse_texname.empty())
			paths.albedo = dir + "/" + mtl.diffuse_texname;
		if (!mtl.alpha_texname.empty())
			paths.opacity = dir + "/" + mtl.alpha_texname;
		if (!mtl.bump_texname.empty())
			paths.normal = dir + "/" + mtl.bump_texname;
		if (!mtl.specular_highlight_texname.empty())
			paths.roughness = dir + "/" + mtl.specular_highlight_texname;
		if (!mtl.ambient_texname.empty())
			paths.metalness = dir + "/" + mtl.ambient_texname;

		loadTexturesToStandardMaterial(paths, material, flipUvX != flipUvY);

		for (const std::vector<MaterialTexture>& stageTextures : material->getTextures())
			for (const MaterialTexture& matTex : stageTextures)
				sceneTextures.push_back(matTex.tex);

		materials.push_back(material);
	}

	sceneMaterials.insert(sceneMaterials.end(), materials.begin(), materials.end());

	if (attrib.vertices.size() % 3 != 0)
	{
		PLOG_ERROR << "Error loading mesh '" << name << "'. \"attrib.vertices.size() % 3 != 0\"";
		return false;
	}

	unsigned int startIndex = 0;
	unsigned int shapeStartIndex = 0;
	mesh_data.vertexData.resize(attrib.vertices.size() / 3);
	mesh_data.submeshes.clear();

	// Loop over shapes
	for (size_t s = 0; s < shapes.size(); s++)
	{
		const tinyobj::shape_t& shape = shapes[s];
		const size_t numIndices = shape.mesh.indices.size();

		constexpr size_t NUM_VERTICES_PER_FACE = 3; // Only triangles are supported now.

		mesh_data.indexData.resize(shapeStartIndex + numIndices);

		int materialId = -1;
		SubmeshData* currentSubmesh = nullptr;

		// Loop over faces
		const size_t numFaces = numIndices / NUM_VERTICES_PER_FACE;
		for (size_t f = 0; f < numFaces; f++)
		{
			if (shape.mesh.num_face_vertices[f] != NUM_VERTICES_PER_FACE)
			{
				PLOG_ERROR << "Error loading mesh '" << name << "'. Only 3 vertices per face (triangles) are supported now.";
				return false;
			}

			// Create a new submesh for each new material used in shape
			if (shape.mesh.material_ids[f] != materialId || currentSubmesh == nullptr)
			{
				SubmeshData submesh;
				submesh.enabled = true;
				submesh.material = nullptr;
				materialId = shape.mesh.material_ids[f];
				if (materialId >= 0)
				{
					if (materialId < materials.size())
						submesh.material = materials[materialId];
					else
						PLOG_WARNING << "Shape '" << shape.name << "' in mesh '" << name << "' has material id outside of materials array bounds. materialId: " << materialId << ", materials.size(): " << materials.size();
				}
				else if (materials.size() > 0)
					PLOG_WARNING << "Shape material id <0 despite model having loaded materials. Model: '" << name << "', Shape: '" << shape.name << "'";
				std::string materialNameSuffix = materialId < 0 ? "" : ("__" + mtls[materialId].name);
				submesh.name = shape.name + materialNameSuffix;
				submesh.startIndex = startIndex;
				submesh.numIndices = 0;
				submesh.startVertex = 0;
				mesh_data.submeshes.push_back(submesh);
				currentSubmesh = &mesh_data.submeshes[mesh_data.submeshes.size() - 1];
			}

			// Loop over vertices in the face.
			for (size_t v = 0; v < NUM_VERTICES_PER_FACE; v++)
			{
				size_t i = f * NUM_VERTICES_PER_FACE + v;
				tinyobj::index_t idx = shape.mesh.indices[i];

				mesh_data.indexData[shapeStartIndex + i] = idx.vertex_index;
				StandardVertexData& vertex = mesh_data.vertexData[idx.vertex_index];

				vertex.position.x = attrib.vertices[3 * idx.vertex_index + 0] * importScale;
				vertex.position.y = attrib.vertices[3 * idx.vertex_index + 1] * importScale;
				vertex.position.z = attrib.vertices[3 * idx.vertex_index + 2] * importScale;

				vertex.normal.x = attrib.normals[3 * idx.normal_index + 0];
				vertex.normal.y = attrib.normals[3 * idx.normal_index + 1];
				vertex.normal.z = attrib.normals[3 * idx.normal_index + 2];

				float uvX = attrib.texcoords[2 * idx.texcoord_index + 0];
				float uvY = attrib.texcoords[2 * idx.texcoord_index + 1];
				vertex.uv.x = flipUvX ? 1.0f - uvX : uvX;
				vertex.uv.y = flipUvY ? 1.0f - uvY : uvY;

				XMFLOAT4 colorF4;
				colorF4.x = attrib.colors[3 * idx.vertex_index + 0];
				colorF4.y = attrib.colors[3 * idx.vertex_index + 1];
				colorF4.z = attrib.colors[3 * idx.vertex_index + 2];
				colorF4.w = attrib.texcoord_ws.size() == 0 ? 1.0f : attrib.texcoord_ws[idx.vertex_index];
				vertex.color =
					(((unsigned int)(colorF4.x * 255u)) & 0xFFu) << 0 |
					(((unsigned int)(colorF4.y * 255u)) & 0xFFu) << 8 |
					(((unsigned int)(colorF4.z * 255u)) & 0xFFu) << 16 |
					(((unsigned int)(colorF4.w * 255u)) & 0xFFu) << 24;
			}

			startIndex += NUM_VERTICES_PER_FACE;
			currentSubmesh->numIndices += NUM_VERTICES_PER_FACE;
		}

		shapeStartIndex = startIndex;
	}

	auto finishProcessing = std::chrono::high_resolution_clock::now();

	PLOG_INFO << "Processing mesh '" << name << "' successful. It took " << (finishProcessing - finishLoadTime).count() / 1e9 << " seconds.";

	return true;
}

bool AssetManager::loadMesh2(const std::string& name, MeshData& out_mesh_data)
{
	if (!modelsIni.has(name))
	{
		PLOG_ERROR << "No model found with name: " << name;
		return false;
	}

	std::string path = modelsIni[name]["path"];
	std::string dir = std::filesystem::path(path).parent_path().u8string();
	float importScale = modelsIni[name]["scale"].length() > 0 ? std::stof(modelsIni[name]["scale"]) : 1.0f;
	bool flipUvX = modelsIni[name]["flipUvX"] == "yes";
	bool flipUvY = modelsIni[name]["flipUvY"] == "yes";

	PLOG_INFO << "Loading mesh '" << name << "' from file: " << path.c_str();

	objl::Loader loader;
	auto startLoadTime = std::chrono::high_resolution_clock::now();
	if (!loader.LoadFile(path))
	{
		PLOG_ERROR << "Couldn't load model file at path: " << path;
		return false;
	}
	auto finishLoadTime = std::chrono::high_resolution_clock::now();
	PLOG_INFO << "Loading mesh '" << name << "' successful. It took " << (finishLoadTime - startLoadTime).count() / 1e9 << " seconds. Processing...";

	unsigned int startIndex = 0;
	unsigned int startVertex = 0;
	out_mesh_data.submeshes.resize(loader.LoadedMeshes.size());

	std::map<std::string, Material*> materialMap;
	int nextMaterialIndex = (int)sceneMaterials.size();

	// Loop over meshes
	for (size_t m = 0; m < loader.LoadedMeshes.size(); m++)
	{
		const objl::Mesh& mesh = loader.LoadedMeshes[m];

		SubmeshData& submesh = out_mesh_data.submeshes[m];
		submesh.name = mesh.MeshName;
		submesh.enabled = true;
		submesh.startIndex = startIndex;
		submesh.numIndices = (unsigned int)mesh.Indices.size();
		submesh.startVertex = startVertex;
		submesh.material = nullptr;

		// Handle material
		const objl::Material& mtl = mesh.MeshMaterial;
		if (!mtl.name.empty() && mtl.name != "none")
		{
			if (materialMap.find(mtl.name) != materialMap.end())
			{
				submesh.material = materialMap[mtl.name];
			}
			else
			{
				Material* material = new Material(name + "__" + mtl.name, standardShaders);

				MaterialTexturePaths paths;
				if (!mtl.map_Kd.empty())
					paths.albedo = dir + "/" + mtl.map_Kd;
				if (!mtl.map_d.empty())
					paths.opacity = dir + "/" + mtl.map_d;
				if (!mtl.map_bump.empty())
					paths.normal = dir + "/" + mtl.map_bump;
				if (!mtl.map_Ns.empty())
					paths.roughness = dir + "/" + mtl.map_Ns;
				if (!mtl.map_Ka.empty())
					paths.metalness = dir + "/" + mtl.map_Ka;

				loadTexturesToStandardMaterial(paths, material, flipUvX != flipUvY);

				for (const std::vector<MaterialTexture>& stageTextures : material->getTextures())
					for (const MaterialTexture& matTex : stageTextures)
						sceneTextures.push_back(matTex.tex);

				sceneMaterials.push_back(material);
				submesh.material = material;
				materialMap[mtl.name] = material;
			}
		}
		else if (loader.LoadedMaterials.size() > 0)
		{
			PLOG_WARNING << "Submesh doesn't have material despite model has loaded materials. Model: '" << name << "', Mesh: '" << mesh.MeshName << "'";
		}

		// Handle indices
		if (mesh.Indices.size() % 3 != 0)
			PLOG_WARNING << "Number of indices is not divisible by 3. Only triangles are supported now! Model: '" << name << "', Mesh: '" << mesh.MeshName << "'";
		out_mesh_data.indexData.resize(startIndex + submesh.numIndices);
		std::copy(mesh.Indices.begin(), mesh.Indices.end(), out_mesh_data.indexData.begin() + startIndex);

		// Handle vertices
		out_mesh_data.vertexData.resize(startVertex + mesh.Vertices.size());
		for (size_t v = 0; v < mesh.Vertices.size(); v++)
		{
			const objl::Vertex& vert = mesh.Vertices[v];
			StandardVertexData& outVert = out_mesh_data.vertexData[startVertex + v];
			outVert.position = XMFLOAT3(vert.Position.X * importScale, vert.Position.Y * importScale, vert.Position.Z * importScale);
			outVert.normal = XMFLOAT3(vert.Normal.X, vert.Normal.Y, vert.Normal.Z);
			outVert.uv = XMFLOAT2(
				flipUvX ? 1.0f - vert.TextureCoordinate.X : vert.TextureCoordinate.X,
				flipUvY ? 1.0f - vert.TextureCoordinate.Y : vert.TextureCoordinate.Y);
			outVert.color = 0xFFFFFFFFu;
		}

		startIndex += submesh.numIndices;
		startVertex += (unsigned int)mesh.Vertices.size();
	}

	auto finishProcessing = std::chrono::high_resolution_clock::now();

	PLOG_INFO << "Processing mesh '" << name << "' successful. It took " << (finishProcessing - finishLoadTime).count() / 1e9 << " seconds.";

	return true;
}

bool AssetManager::loadMeshToMeshRenderer(const std::string& name, MeshRenderer& mesh_renderer, LoadExecutionMode lem)
{
	auto load = [&, name]
	{
		MeshData meshData;
		if (!loadMesh2(name, meshData))
			return false;
		mesh_renderer.load(meshData);
		return true;
	};

	if (lem == LoadExecutionMode::ASYNC && ASYNC_LOADING_ENABLED)
	{
		tp->enqueue(load);
		return true;
	}
	else
		return load();
}

void AssetManager::loadScene(const std::string& scene_file)
{
	currentSceneIniFile = std::make_unique<mINI::INIFile>(scene_file);
	if (!currentSceneIniFile->read(currentSceneIni))
	{
		PLOG_ERROR << "Couldn't find scene file: " << scene_file;
		return;
	}
	currentSceneIniFilePath = scene_file;

	std::map<std::string, ITexture*> texturePathMap;

	for (auto& sceneElem : currentSceneIni)
	{
		auto strToVector = [](std::string s)
		{
			if (s.length() == 0)
				return XMVectorZero();
			char delimiter = ',';
			size_t pos = s.find(delimiter);
			float x = std::stof(s.substr(0, pos));
			if (pos == std::string::npos) // No comma
				return XMVectorSet(x, 0, 0, 0);
			s.erase(0, pos + 1);
			pos = s.find(delimiter);
			float y = std::stof(s.substr(0, pos));
			if (pos == std::string::npos) // Only 1 comma
				return XMVectorSet(x, y, 0, 0);
			s.erase(0, pos + 1);
			pos = s.find(delimiter);
			assert(pos == std::string::npos); // More than 2 comma
			float z = std::stof(s.substr(0, pos));
			return XMVectorSet(x, y, z, 0);
		};

		auto& elemProperties = currentSceneIni[sceneElem.first];

		if (elemProperties["type"] == "model")
		{
			std::string materialName = elemProperties["material"];
			Material* material = nullptr;
			auto it = std::find_if(sceneMaterials.begin(), sceneMaterials.end(), [&materialName](Material* m) { return materialName == m->name; });
			if (it != sceneMaterials.end())
				material = *it;
			else
			{
				if (!materialsIni.has(materialName))
				{
					PLOG_ERROR << "No material found with name: " << materialName;
					material = new Material(materialName.c_str(), { drv->getErrorShader(), drv->getErrorShader() });
				}
				else
				{
					material = new Material(materialName.c_str(), standardShaders);

					MaterialTexturePaths texturePaths;
					texturePaths.albedo = materialsIni[materialName]["albedo_tex"];
					texturePaths.opacity = materialsIni[materialName]["opacity_tex"];
					texturePaths.normal = materialsIni[materialName]["normal_tex"];
					texturePaths.roughness = materialsIni[materialName]["roughness_tex"];
					texturePaths.metalness = materialsIni[materialName]["metalness_tex"];

					loadTexturesToStandardMaterial(texturePaths, material, false);

					for (const std::vector<MaterialTexture>& stageTextures : material->getTextures())
						for (const MaterialTexture& matTex : stageTextures)
							sceneTextures.push_back(matTex.tex);
				}
				sceneMaterials.push_back(material);
			}

			MeshRenderer* mr = new MeshRenderer(sceneElem.first.c_str(), material, standardInputLayout);
			sceneMeshRenderers.push_back(mr);

			std::string modelName = elemProperties["model"];
			loadMeshToMeshRenderer(modelName, *mr);

			Transform tr;
			tr.position = strToVector(elemProperties["position"]);
			tr.rotation = strToVector(elemProperties["rotation"]) * DEG_TO_RAD;
			tr.scale = elemProperties["scale"].length() > 0 ? std::stof(elemProperties["scale"]) : 1.0f;
			mr->setTransform(tr);
		}
		else if (elemProperties["type"] == "camera")
		{
			if (elemProperties.has("position"))
				wr->getCamera().SetEye(strToVector(elemProperties["position"]));
			if (elemProperties.has("rotation"))
			{
				XMVECTOR rotVec = strToVector(elemProperties["rotation"]) * DEG_TO_RAD;
				wr->getCamera().SetRotation(rotVec.m128_f32[0], rotVec.m128_f32[1]);
			}
		}
	}
}

void AssetManager::unloadCurrentScene()
{
	for (MeshRenderer* mr : sceneMeshRenderers)
		delete mr;
	sceneMeshRenderers.clear();
	for (Material* m : sceneMaterials)
		delete m;
	sceneMaterials.clear();
	for (ITexture* t : sceneTextures)
		delete t;
	sceneTextures.clear();
}

void AssetManager::initInis()
{
	const char* materialsIniPath = "Assets/materials.ini";
	materialsIniFile = std::make_unique<mINI::INIFile>(materialsIniPath);
	if (!materialsIniFile->read(materialsIni))
	{
		PLOG_ERROR << "Couldn't materials ini file: " << materialsIniPath;
		return;
	}

	const char* modelsIniPath = "Assets/models.ini";
	modelsIniFile = std::make_unique<mINI::INIFile>(modelsIniPath);
	if (!modelsIniFile->read(modelsIni))
	{
		PLOG_ERROR << "Couldn't models ini file: " << modelsIniPath;
		return;
	}
}

void AssetManager::initShaders()
{
	ShaderSetDesc errorShaderDesc("Error", "Source/Shaders/Error.shader");
	errorShaderDesc.shaderFuncNames[(int)ShaderStage::VS] = "ErrorVS";
	errorShaderDesc.shaderFuncNames[(int)ShaderStage::PS] = "ErrorPS";
	drv->setErrorShaderDesc(errorShaderDesc);

	ShaderSetDesc shaderDesc("Standard", "Source/Shaders/Standard.shader");
	shaderDesc.shaderFuncNames[(int)ShaderStage::VS] = "StandardForwardVS";
	shaderDesc.shaderFuncNames[(int)ShaderStage::PS] = "StandardOpaqueForwardPS";
	standardShaders[(int)RenderPass::FORWARD] = drv->createShaderSet(shaderDesc);
	shaderDesc.shaderFuncNames[(int)ShaderStage::VS] = "StandardDepthOnlyVS";
	shaderDesc.shaderFuncNames[(int)ShaderStage::PS] = "StandardOpaqueDepthOnlyPS";
	standardShaders[(int)RenderPass::DEPTH] = drv->createShaderSet(shaderDesc);

	constexpr unsigned int NUM_STANDARD_INPUT_LAYOUT_ELEMENTS = 4;
	InputLayoutElementDesc standardInputLayoutDesc[NUM_STANDARD_INPUT_LAYOUT_ELEMENTS] =
	{
		{ VertexInputSemantic::POSITION, 0, TexFmt::R32G32B32_FLOAT },
		{ VertexInputSemantic::NORMAL,   0, TexFmt::R32G32B32_FLOAT },
		{ VertexInputSemantic::COLOR,    0, TexFmt::R8G8B8A8_UNORM  },
		{ VertexInputSemantic::TEXCOORD, 0, TexFmt::R32G32_FLOAT    },
	};
	standardInputLayout = drv->createInputLayout(standardInputLayoutDesc,
		NUM_STANDARD_INPUT_LAYOUT_ELEMENTS, standardShaders[(int)RenderPass::FORWARD]);
}

void AssetManager::initDefaultAssets()
{
	ITexture* stubColor = loadTextureFromPng("Assets/Textures/gray_base.png", true, LoadExecutionMode::SYNC);
	ITexture* stubNormal = loadTextureFromPng("Assets/Textures/flatnorm_dielectric_halfrough_nrm.png", false, LoadExecutionMode::SYNC);
	engineTextures.push_back(stubColor);
	engineTextures.push_back(stubNormal);
	defaultTextures[(int)MaterialTexture::Purpose::COLOR] = stubColor;
	defaultTextures[(int)MaterialTexture::Purpose::NORMAL] = stubNormal;
	defaultTextures[(int)MaterialTexture::Purpose::OTHER] = stubColor;

	MeshData defaultMesh;
	loadMesh2("Box", defaultMesh);
	BufferDesc vbDesc("defaultMeshVb", sizeof(defaultMesh.vertexData[0]), (unsigned int)defaultMesh.vertexData.size(), ResourceUsage::DEFAULT, BIND_VERTEX_BUFFER);
	vbDesc.initialData = defaultMesh.vertexData.data();
	defaultMeshVb.reset(drv->createBuffer(vbDesc));
	unsigned int indexByteSize = ::get_byte_size_for_texfmt(drv->getIndexFormat());
	BufferDesc ibDesc("defaultMeshIb", indexByteSize, (unsigned int)defaultMesh.indexData.size(), ResourceUsage::DEFAULT, BIND_INDEX_BUFFER);
	ibDesc.initialData = defaultMesh.indexData.data();
	defaultMeshIb.reset(drv->createBuffer(ibDesc));

	defaultInputLayout = standardInputLayout;
}