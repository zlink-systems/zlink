extern alias etcdprovider;

global using Xunit;
global using Zlink.Framework.LocationProvider;
global using EtcdClient = etcdprovider::Zlink.Framework.Locations.Etcd.EtcdClient;
global using EtcdCursor = etcdprovider::Zlink.Framework.Locations.Etcd.EtcdCursor;
global using EtcdEncoding = etcdprovider::Zlink.Framework.Locations.Etcd.EtcdEncoding;
global using EtcdSnapshot = etcdprovider::Zlink.Framework.Locations.Etcd.EtcdSnapshot;
global using ZLinkEtcdCredentials = etcdprovider::Zlink.Framework.Locations.Etcd.ZLinkEtcdCredentials;
global using ZLinkEtcdLocationStore = etcdprovider::Zlink.Framework.Locations.Etcd.ZLinkEtcdLocationStore;
