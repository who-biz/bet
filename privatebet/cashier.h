
void BET_check_notary_status();
int32_t BET_send_status(struct cashier *cashier_info);
int32_t BET_cashier_backend(cJSON *argjson,struct cashier *cashier_info);
void BET_cashier_loop(void * _ptr);