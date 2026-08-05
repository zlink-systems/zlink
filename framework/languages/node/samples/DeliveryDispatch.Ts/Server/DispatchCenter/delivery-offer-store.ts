import * as fs from 'node:fs';
import * as path from 'node:path';

type DeliveryOfferStatus = 'Offered' | 'Accepted' | 'Rejected' | 'Expired' | 'Failed';

type DeliveryOffer = {
  deliveryId: string;
  customerId: string;
  pickupAddress: string;
  dropoffAddress: string;
  courierId: string;
  attempt: number;
  deadline: number;
  status: DeliveryOfferStatus;
};

class DeliveryOfferStore {
  private readonly filePath: string;
  private readonly offers = new Map<string, DeliveryOffer>();

  constructor(workDir: string) {
    fs.mkdirSync(workDir, { recursive: true });
    this.filePath = path.join(workDir, 'delivery-offers.json');
    this.load();
  }

  get(deliveryId: string): DeliveryOffer | undefined {
    const offer = this.offers.get(deliveryId);
    return offer === undefined ? undefined : { ...offer };
  }

  offered(): DeliveryOffer[] {
    return [...this.offers.values()]
      .filter((offer) => offer.status === 'Offered')
      .map((offer) => ({ ...offer }));
  }

  save(offer: DeliveryOffer): void {
    this.offers.set(offer.deliveryId, { ...offer });
    this.persist();
  }

  private load(): void {
    if (!fs.existsSync(this.filePath)) return;
    const values = JSON.parse(fs.readFileSync(this.filePath, 'utf8')) as DeliveryOffer[];
    if (!Array.isArray(values)) throw new Error('Delivery offer store must contain an array.');
    for (const value of values) this.offers.set(value.deliveryId, value);
  }

  private persist(): void {
    const temporary = `${this.filePath}.tmp`;
    fs.writeFileSync(temporary, `${JSON.stringify([...this.offers.values()], null, 2)}\n`);
    fs.renameSync(temporary, this.filePath);
  }
}

export { DeliveryOfferStore };
export type { DeliveryOffer, DeliveryOfferStatus };
