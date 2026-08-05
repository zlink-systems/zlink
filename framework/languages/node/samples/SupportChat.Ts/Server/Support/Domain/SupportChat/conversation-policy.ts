class ConversationPolicy {
  readonly maximumParticipants = 2;
  readonly maximumMessageLength = 1000;

  validateMessage(text: string): void {
    if (text.trim().length === 0 || text.length > this.maximumMessageLength) {
      throw new Error(`Chat message text must contain between 1 and ${this.maximumMessageLength} characters.`);
    }
  }
}

export { ConversationPolicy };
