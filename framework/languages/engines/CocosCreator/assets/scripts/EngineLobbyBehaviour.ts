import { _decorator, Component, Label } from 'cc';
import { EngineLobbyClient } from './EngineLobbyClient';

const { ccclass, property } = _decorator;

@ccclass('EngineLobbyBehaviour')
export class EngineLobbyBehaviour extends Component {
  @property
  endpoint = 'ws://127.0.0.1:22700';

  @property
  playerName = 'cocos-player';

  @property
  firstChat = 'hello from Cocos Creator';

  @property(Label)
  statusLabel: Label | null = null;

  private client: EngineLobbyClient | null = null;
  private dispatching = false;

  start(): void {
    if (this.statusLabel === null) this.statusLabel = this.node.addComponent(Label);
    this.setStatus('Engine Lobby: connecting');
    this.client = new EngineLobbyClient(
      this.endpoint,
      (status) => this.setStatus(status),
      (notification) => this.setStatus(`${notification.name}: ${notification.text}`)
    );
    void this.client
      .start(this.playerName)
      .then(() => this.client?.sendChat(this.firstChat))
      .catch((error) => {
        this.setStatus(`Engine Lobby failed: ${String(error)}`);
      });
  }

  update(): void {
    if (this.client === null || this.dispatching) return;
    this.dispatching = true;
    void this.client
      .dispatch()
      .catch((error) => this.setStatus(`Engine Lobby dispatch failed: ${String(error)}`))
      .finally(() => {
        this.dispatching = false;
      });
  }

  onDestroy(): void {
    if (this.client === null) return;
    void this.client.close().catch((error) => {
      console.error('Engine Lobby close failed:', error);
    });
    this.client = null;
  }

  private setStatus(status: string): void {
    if (this.statusLabel !== null) this.statusLabel.string = status;
    console.log(`Engine Lobby: ${status}`);
  }
}
