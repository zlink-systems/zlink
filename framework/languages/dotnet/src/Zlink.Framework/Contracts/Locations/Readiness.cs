namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// 일반 application health check용 boolean 질의. 확인 불가(store 장애 포함)는
/// false다 — readiness에서 "모른다"는 "준비 안 됐다"와 같다. 이는 "인프라
/// 장애는 예외" 일반 규칙의 명시된 예외이며, 현재 샘플마다 반복되는
/// try/catch → false 처리를 계약이 흡수한 것이다. 장애 원인 진단은
/// <see cref="IZLinkLocationRuntimeQuery.GetStatusAsync"/>가 담당한다.
/// </summary>
public interface IZLinkLocationReadiness
{
    ValueTask<bool> IsPeerReadyAsync(
        string meshName,
        ZLinkLocationRole role,
        RoutingId? nodeRid = null,
        CancellationToken cancellationToken = default);
}
