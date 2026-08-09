#include "Components.h"

namespace Engine
{
    DirectX::XMMATRIX ComputeWorldMatrix(const Transform& transform)
    {
        const DirectX::XMVECTOR rotation = DirectX::XMLoadFloat4(&transform.rotation);
        const DirectX::XMVECTOR position = DirectX::XMLoadFloat3(&transform.position);
        return DirectX::XMMatrixScaling(transform.scale, transform.scale, transform.scale) *
               DirectX::XMMatrixRotationQuaternion(rotation) * DirectX::XMMatrixTranslationFromVector(position);
    }
}
