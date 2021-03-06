#include "InputLayout.h"

#include <Common.h>

#include <assert.h>
#include <vector>

#include "ShaderSet.h"

using namespace drv_d3d11;

static const char* get_semantic_string(VertexInputSemantic semantic)
{
	switch (semantic)
	{
	case VertexInputSemantic::POSITION:
		return "POSITION";
	case VertexInputSemantic::NORMAL:
		return "NORMAL";
	case VertexInputSemantic::TANGENT:
		return "TANGENT";
	case VertexInputSemantic::COLOR:
		return "COLOR";
	case VertexInputSemantic::TEXCOORD:
		return "TEXCOORD";
	default:
		assert(false);
		return nullptr;
	}
}

InputLayout::InputLayout(const InputLayoutElementDesc* descs, unsigned int num_descs, const ShaderSet& shader_set)
{
	std::vector<D3D11_INPUT_ELEMENT_DESC> ieds;
	ieds.resize(num_descs);
	unsigned int byteSizeSoFar = 0;
	for (unsigned int i = 0; i < num_descs; i++)
	{
		ieds[i].SemanticName = get_semantic_string(descs[i].semantic);
		ieds[i].SemanticIndex = descs[i].semanticIndex;
		ieds[i].Format = (DXGI_FORMAT)descs[i].format;
		ieds[i].InputSlot = 0;
		ieds[i].AlignedByteOffset = byteSizeSoFar;
		ieds[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		ieds[i].InstanceDataStepRate = 0;

		byteSizeSoFar += get_byte_size_for_texfmt(descs[i].format);
	}

	ID3DBlob* vsBlob = shader_set.getVsBlob();
	assert(vsBlob != nullptr);
	HRESULT hr = Driver::get().getDevice().CreateInputLayout(ieds.data(), num_descs,
		vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
	assert(SUCCEEDED(hr));

	id = Driver::get().registerInputLayout(this);
}

InputLayout::~InputLayout()
{
	SAFE_RELEASE(inputLayout);
	Driver::get().unregisterInputLayout(id);
}
