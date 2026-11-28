namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueFixtureTests
{
    [Fact]
    public void FixtureSectionsContainAllContractCases()
    {
        Assert.Equal(
            12,
            AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "ticketVectors").Count()
        );
        Assert.Equal(
            22,
            AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "queueScenarios").Count()
        );
        Assert.Equal(
            10,
            AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "nodeScenarios").Count()
        );
        Assert.Equal(
            7,
            AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "startupScenarios").Count()
        );
        Assert.Equal(
            11,
            AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "connectorScenarios").Count()
        );
        Assert.Equal(8, AdmissionQueueWireTests.Messages().Count());
        Assert.Equal(9, AdmissionQueueWireTests.InvalidMessages().Count());
    }
}
