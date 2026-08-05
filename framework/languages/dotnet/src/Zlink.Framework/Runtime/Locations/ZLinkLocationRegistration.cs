namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Raw location store registration collected by the options builder.
/// </summary>
internal sealed class ZLinkLocationRegistration
{
    private ZLinkInMemoryLocationStore? _inMemoryStore;
    private IZLinkLocationRepository? _repository;
    private IZLinkRelocationRepository? _relocationRepository;

    public bool UseInMemoryStores { get; set; }

    /// <summary>The opaque provider instance used by the private repository
    /// adapter.</summary>
    public IZLinkLocationStore? StoreInstance { get; set; }

    public IZLinkRelocationStore? RelocationStoreInstance { get; set; }

    public ZLinkLocationOptions Options { get; } = new();

    public bool Enabled =>
        UseInMemoryStores || StoreInstance is not null || _repository is not null;

    internal bool HasExplicitStore =>
        StoreInstance is not null || _repository is not null;

    internal IZLinkLocationRepository? ResolveStore()
    {
        if (_repository is not null) return _repository;
        if (StoreInstance is not null)
            return _repository ??= new ZLinkProviderLocationRepository(StoreInstance);
        if (!UseInMemoryStores) return null;
        return _inMemoryStore ??= new ZLinkInMemoryLocationStore();
    }

    internal IZLinkRelocationRepository? ResolveRelocationStore()
    {
        if (_relocationRepository is not null) return _relocationRepository;
        if (RelocationStoreInstance is null) return null;
        return _relocationRepository ??=
            new ZLinkProviderRelocationRepository(RelocationStoreInstance);
    }

    internal void SetTestRepository(IZLinkLocationRepository repository)
    {
        if (StoreInstance is not null || _repository is not null)
            throw new ZLinkConfigurationException(
                "A Location Store is already registered.");
        _repository = repository;
    }

    internal void SetTestRelocationRepository(
        IZLinkRelocationRepository repository)
    {
        if (RelocationStoreInstance is not null
            || _relocationRepository is not null)
        {
            throw new ZLinkConfigurationException(
                "A Relocation Store is already registered.");
        }
        _relocationRepository = repository;
    }
}
