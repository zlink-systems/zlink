using ShoppingMall.Server.Shared.Domain;
using ShoppingMall.Shared.Contracts;

namespace ShoppingMall.Server.Shared.Contracts;

public static class OrderContractMapper
{
    public static OrderState ToContract(OrderProjectionState state)
    {
        return new OrderState(
            state.OrderId,
            state.Status.ToString(),
            state.ShippingAddressId,
            state.ReservationId,
            state.PaymentId,
            state.Reason,
            state.Amount,
            state.Currency,
            state.UpdatedAtUnixMs);
    }
}
