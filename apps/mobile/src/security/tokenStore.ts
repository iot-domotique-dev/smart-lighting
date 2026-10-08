import * as SecureStore from 'expo-secure-store';

const TOKEN_KEY = 'smart_lighting_core_bearer_token';

export const tokenStore = {
  read: () => SecureStore.getItemAsync(TOKEN_KEY),
  save: (token: string) => SecureStore.setItemAsync(TOKEN_KEY, token),
  clear: () => SecureStore.deleteItemAsync(TOKEN_KEY),
};
